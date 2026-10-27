#include "gtg_https_server.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_https_server.h"
#include "esp_littlefs.h"
#include "cJSON.h"

#include "gtg_wifi_station.h"

#define WWW_PARTITION_LABEL   "www"
#define WWW_BASE_PATH         "/www"
#define WWW_INDEX_FILE        "index.html"
#define WWW_GZIP_SUFFIX       ".gz"

#define FILE_CHUNK_SIZE       1024
#define API_BODY_MAX_LEN      256
#define FILE_PATH_MAX         (sizeof(WWW_BASE_PATH) + CONFIG_HTTPD_MAX_URI_LEN + sizeof(WWW_INDEX_FILE) + sizeof(WWW_GZIP_SUFFIX))

static const char *TAG = "https server";

static httpd_handle_t s_server = NULL;
static bool s_www_mounted = false;

/* Embedded from cred/ through EMBED_TXTFILES in main/CMakeLists.txt */
extern const unsigned char servercert_start[] asm("_binary_servercert_pem_start");
extern const unsigned char servercert_end[]   asm("_binary_servercert_pem_end");
extern const unsigned char prvtkey_start[]    asm("_binary_prvtkey_pem_start");
extern const unsigned char prvtkey_end[]      asm("_binary_prvtkey_pem_end");

typedef struct {
    const char *ext;
    const char *type;
} mime_type_t;

static const mime_type_t MIME_TYPES[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".htm",  "text/html; charset=utf-8" },
    { ".css",  "text/css; charset=utf-8" },
    { ".js",   "application/javascript; charset=utf-8" },
    { ".mjs",  "application/javascript; charset=utf-8" },
    { ".json", "application/json; charset=utf-8" },
    { ".svg",  "image/svg+xml" },
    { ".png",  "image/png" },
    { ".jpg",  "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".ico",  "image/x-icon" },
    { ".txt",  "text/plain; charset=utf-8" },
};

static const char *get_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (ext != NULL) {
        for (size_t i = 0; i < sizeof(MIME_TYPES) / sizeof(MIME_TYPES[0]); i++) {
            if (strcasecmp(ext, MIME_TYPES[i].ext) == 0) {
                return MIME_TYPES[i].type;
            }
        }
    }
    return "application/octet-stream";
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Serves files from the www partition. Text files are stored as <name>.gz by
 * tools/gzip_web.py and are sent with Content-Encoding: gzip */
static esp_err_t static_file_get_handler(httpd_req_t *req)
{
    char path[FILE_PATH_MAX];

    /* strip the query string */
    size_t uri_len = strcspn(req->uri, "?#");

    /* don't allow escaping the www directory */
    if (uri_len == 0 || req->uri[0] != '/' || memmem(req->uri, uri_len, "..", 2) != NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    int len = snprintf(path, sizeof(path), WWW_BASE_PATH "%.*s%s",
                       (int)uri_len, req->uri,
                       req->uri[uri_len - 1] == '/' ? WWW_INDEX_FILE : "");
    if (len < 0 || (size_t)len + sizeof(WWW_GZIP_SUFFIX) > sizeof(path)) {
        return httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "URI too long");
    }

    /* the mime type comes from the original name, not the .gz one */
    const char *mime_type = get_mime_type(path);

    strcat(path, WWW_GZIP_SUFFIX);
    bool gzipped = file_exists(path);
    if (!gzipped) {
        path[len] = '\0';
        if (!file_exists(path)) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        }
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "failed to open %s", path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
    }

    httpd_resp_set_type(req, mime_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (gzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    char chunk[FILE_CHUNK_SIZE];
    size_t read_len;
    esp_err_t err = ESP_OK;

    while ((read_len = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        err = httpd_resp_send_chunk(req, chunk, read_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to send %s", path);
            break;
        }
    }

    fclose(file);

    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);

    cJSON_free(body);
    return err;
}

static const char *station_state_name(wifi_station_state_t state)
{
    switch (state) {
    case WIFI_STATION_CONNECTING: return "connecting";
    case WIFI_STATION_CONNECTED:  return "connected";
    case WIFI_STATION_FAILED:     return "failed";
    default:                      return "idle";
    }
}

/* Responds with {"state": "idle|connecting|connected|failed", "reason": <wifi_err_reason_t>} */
static esp_err_t send_wifi_status(httpd_req_t *req, const char *status)
{
    uint8_t reason;
    wifi_station_state_t state = wifi_station_get_state(&reason);

    cJSON *json = cJSON_CreateObject();
    if (json == NULL
        || cJSON_AddStringToObject(json, "state", station_state_name(state)) == NULL
        || cJSON_AddNumberToObject(json, "reason", reason) == NULL) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    esp_err_t err = send_json(req, status, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    return send_wifi_status(req, "200 OK");
}

/* Expects {"ssid": "...", "pass": "..."} and starts connecting without waiting for the
 * result, as the softAP can drop for a moment when it moves to the router's channel.
 * The result is polled through /api/wifi/status */
static esp_err_t wifi_connect_post_handler(httpd_req_t *req)
{
    char body[API_BODY_MAX_LEN + 1];

    if (req->content_len == 0 || req->content_len > API_BODY_MAX_LEN) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body length");
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);

    /* the body contains the password */
    memset(body, 0, sizeof(body));

    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "ssid"));
    char *pass = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "pass"));

    esp_err_t err = wifi_station_connect(ssid, pass);

    if (pass != NULL) {
        memset(pass, 0, strlen(pass));
    }
    cJSON_Delete(json);

    if (err == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID must be 1-32 and password 8-63 characters");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Already connecting");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to connect");
    }

    return send_wifi_status(req, "202 Accepted");
}

static const httpd_uri_t api_uris[] = {
    {
        .uri       = "/api/wifi/status",
        .method    = HTTP_GET,
        .handler   = wifi_status_get_handler,
    },
    {
        .uri       = "/api/wifi/connect",
        .method    = HTTP_POST,
        .handler   = wifi_connect_post_handler,
    },
};

/* Must stay registered last. URI handlers are matched in registration order,
 * so register API endpoints before this catch-all */
static const httpd_uri_t static_file_uri = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = static_file_get_handler,
};

static esp_err_t www_mount(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = WWW_BASE_PATH,
        .partition_label = WWW_PARTITION_LABEL,
        .format_if_mount_failed = false,
        .read_only = true,
    };

    ESP_RETURN_ON_ERROR(esp_vfs_littlefs_register(&conf), TAG, "failed to mount %s partition", WWW_PARTITION_LABEL);
    s_www_mounted = true;

    size_t total = 0, used = 0;
    if (esp_littlefs_info(WWW_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "%s partition mounted, used %u of %u bytes",
                 WWW_PARTITION_LABEL, (unsigned)used, (unsigned)total);
    }

    return ESP_OK;
}

static void www_unmount(void)
{
    if (s_www_mounted) {
        esp_vfs_littlefs_unregister(WWW_PARTITION_LABEL);
        s_www_mounted = false;
    }
}

esp_err_t gtg_https_server_init(void)
{
    if (s_server != NULL) {
        ESP_LOGE(TAG, "already running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(www_mount(), TAG, "web files unavailable");

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.uri_match_fn = httpd_uri_match_wildcard;

    /* text files are embedded with a NUL terminator, which mbedTLS expects to be included in the PEM length */
    conf.servercert = servercert_start;
    conf.servercert_len = servercert_end - servercert_start;
    conf.prvtkey_pem = prvtkey_start;
    conf.prvtkey_len = prvtkey_end - prvtkey_start;

    esp_err_t err = httpd_ssl_start(&s_server, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start server");
        s_server = NULL;
        www_unmount();
        return err;
    }

    for (size_t i = 0; i < sizeof(api_uris) / sizeof(api_uris[0]) && err == ESP_OK; i++) {
        err = httpd_register_uri_handler(s_server, &api_uris[i]);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_server, &static_file_uri);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register URI handlers");
        gtg_https_server_deinit();
        return err;
    }

    ESP_LOGI(TAG, "server started on port %d", conf.port_secure);

    return ESP_OK;
}

void gtg_https_server_deinit(void)
{
    if (s_server != NULL) {
        if (httpd_ssl_stop(s_server) != ESP_OK) {
            ESP_LOGE(TAG, "failed to stop server");
        }
        s_server = NULL;
    }

    www_unmount();
}
