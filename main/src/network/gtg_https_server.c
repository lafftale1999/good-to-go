#include "gtg_https_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_https_server.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "cJSON.h"

#include "gtg_wifi_station.h"
#include "gtg_api_keys.h"
#include "gtg_stop_search.h"
#include "gtg_departures.h"
#include "gtg_dev_config.h"
#include "gtg_events.h"

#define WWW_PARTITION_LABEL   "www"
#define WWW_BASE_PATH         "/www"
#define WWW_INDEX_FILE        "index.html"
#define WWW_GZIP_SUFFIX       ".gz"

#define FILE_CHUNK_SIZE       1024
/* fits a target from /api/targets with every field at its full length */
#define API_BODY_MAX_LEN      512
#define WIFI_SCAN_MAX_RESULTS 10
#define HTTPD_STACK_SIZE      16384

/* lets the response reach the browser before the device restarts after a factory reset */
#define FACTORY_RESET_RESTART_DELAY_US  (1000 * 1000)
#define FACTORY_RESET_CONFIRM           "factory-reset"
#define FILE_PATH_MAX         (sizeof(WWW_BASE_PATH) + CONFIG_HTTPD_MAX_URI_LEN + sizeof(WWW_INDEX_FILE) + sizeof(WWW_GZIP_SUFFIX))

/* No I, O, 0 or 1, which are easily mistaken for each other on the LCD. 32 characters, so a
 * random byte picks one without bias */
#define PASSWORD_ALPHABET     "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

/* The session token is sent as a cookie, which the browser keeps and sends with every request.
 * HttpOnly keeps it from the page's scripts, SameSite=Strict from requests other sites start */
#define SESSION_COOKIE        "gtg_session"
#define SESSION_TOKEN_BYTES   32
#define SESSION_TOKEN_LEN     (2 * SESSION_TOKEN_BYTES)
#define SESSION_MAX           4
#define SESSION_TIMEOUT_US    (5LL * 60 * 1000 * 1000)

/* wrong passwords in a row before the login is locked for a while */
#define LOGIN_MAX_FAILURES    5
#define LOGIN_LOCK_US         (30LL * 1000 * 1000)

static const char *TAG = "https server";

static httpd_handle_t s_server = NULL;
static bool s_www_mounted = false;

/* esp_timer time of the last logged in request or login, or of the start when none has come yet */
static volatile int64_t s_last_activity_us = 0;

/* Made at every start. The sessions and the login state are only used from the server task */
static char s_password[GTG_HTTPS_PASSWORD_LEN + 1];

typedef struct {
    char    token[SESSION_TOKEN_LEN + 1];   /*!< hex, empty when the slot is free */
    int64_t last_used_us;
} session_t;

static session_t s_sessions[SESSION_MAX];
static uint8_t s_login_failures = 0;
static int64_t s_login_locked_until_us = 0;

/* Whether the response to the request being handled has a 2xx status. The server handles one
 * request at a time, so one flag serves them all, see set_status() and send_err() */
static bool s_resp_success = true;

/* httpd_resp_set_status() that notes whether the status is a success, as the server can't read
 * it back. Every status in this file is set through here or send_err() */
static esp_err_t set_status(httpd_req_t *req, const char *status)
{
    s_resp_success = status[0] == '2';
    return httpd_resp_set_status(req, status);
}

/* httpd_resp_send_err(), whose codes are all errors */
static esp_err_t send_err(httpd_req_t *req, httpd_err_code_t code, const char *msg)
{
    s_resp_success = false;
    return httpd_resp_send_err(req, code, msg);
}

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
        return send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    int len = snprintf(path, sizeof(path), WWW_BASE_PATH "%.*s%s",
                       (int)uri_len, req->uri,
                       req->uri[uri_len - 1] == '/' ? WWW_INDEX_FILE : "");
    if (len < 0 || (size_t)len + sizeof(WWW_GZIP_SUFFIX) > sizeof(path)) {
        return send_err(req, HTTPD_414_URI_TOO_LONG, "URI too long");
    }

    /* the mime type comes from the original name, not the .gz one */
    const char *mime_type = get_mime_type(path);

    strcat(path, WWW_GZIP_SUFFIX);
    bool gzipped = file_exists(path);
    if (!gzipped) {
        path[len] = '\0';
        if (!file_exists(path)) {
            return send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        }
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "failed to open %s", path);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read file");
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
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    set_status(req, status);
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

/* Responds with {"state": "idle|connecting|connected|failed", "reason": <wifi_err_reason_t>,
 * "ssid": "..."}, the ssid of the connected network, empty when not connected */
static esp_err_t send_wifi_status(httpd_req_t *req, const char *status)
{
    uint8_t reason;
    wifi_station_state_t state = wifi_station_get_state(&reason);

    char ssid[33] = "";
    if (state == WIFI_STATION_CONNECTED) {
        wifi_station_get_ssid(ssid, sizeof(ssid));
    }

    cJSON *json = cJSON_CreateObject();
    if (json == NULL
        || cJSON_AddStringToObject(json, "state", station_state_name(state)) == NULL
        || cJSON_AddNumberToObject(json, "reason", reason) == NULL
        || cJSON_AddStringToObject(json, "ssid", ssid) == NULL) {
        cJSON_Delete(json);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    esp_err_t err = send_json(req, status, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t wifi_status_get_handler(httpd_req_t *req)
{
    return send_wifi_status(req, "200 OK");
}

/* Responds with the runtime flags of the device configuration, {"connected": bool,
 * "time_synced": bool, "keys_verified": bool, "has_departures": bool, "valid_config": bool} */
static esp_err_t send_status(httpd_req_t *req, const char *status)
{
    uint32_t flags = gtg_dev_config_get_flags();

    cJSON *json = cJSON_CreateObject();
    if (json == NULL
        || cJSON_AddBoolToObject(json, "connected", flags & GTG_DEV_FLAG_CONNECTED) == NULL
        || cJSON_AddBoolToObject(json, "time_synced", flags & GTG_DEV_FLAG_TIME_SYNCED) == NULL
        || cJSON_AddBoolToObject(json, "keys_verified", flags & GTG_DEV_FLAG_KEYS_VERIFIED) == NULL
        || cJSON_AddBoolToObject(json, "has_departures", flags & GTG_DEV_FLAG_HAS_DEPARTURES) == NULL
        || cJSON_AddBoolToObject(json, "valid_config", flags & GTG_DEV_FLAG_VALID_CONFIG) == NULL) {
        cJSON_Delete(json);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    esp_err_t err = send_json(req, status, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    return send_status(req, "200 OK");
}

/* The "Done" button. The configuration becomes valid by itself, so nothing changes here, it
 * answers like /api/status with 409 Conflict while something is still missing. A valid one is
 * posted as applied, so the device tells the user how to stop the configuration mode */
static esp_err_t config_apply_post_handler(httpd_req_t *req)
{
    bool valid = gtg_dev_config_get_flags() & GTG_DEV_FLAG_VALID_CONFIG;
    if (valid) {
        gtg_event_t event = { .type = GTG_EV_CONFIG_APPLIED };
        gtg_events_post(&event);
    }

    return send_status(req, valid ? "200 OK" : "409 Conflict");
}

/* Receives and parses a JSON body. On NULL the error response has been sent and
 * *err holds the handler's return value */
static cJSON *receive_json(httpd_req_t *req, esp_err_t *err)
{
    char body[API_BODY_MAX_LEN + 1];

    if (req->content_len == 0 || req->content_len > API_BODY_MAX_LEN) {
        *err = send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body length");
        return NULL;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            *err = ESP_FAIL;
            return NULL;
        }
        received += ret;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);

    /* the body can contain passwords and keys */
    memset(body, 0, sizeof(body));

    if (json == NULL) {
        *err = send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    return json;
}

/* Expects {"ssid": "...", "pass": "..."} and starts connecting without waiting for the
 * result, as the softAP can drop for a moment when it moves to the router's channel.
 * The result is polled through /api/wifi/status */
static esp_err_t wifi_connect_post_handler(httpd_req_t *req)
{
    esp_err_t err;
    cJSON *json = receive_json(req, &err);
    if (json == NULL) {
        return err;
    }

    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "ssid"));
    char *pass = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "pass"));

    err = wifi_station_connect(ssid, pass);

    if (pass != NULL) {
        memset(pass, 0, strlen(pass));
    }
    cJSON_Delete(json);

    if (err == ESP_ERR_INVALID_ARG) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "SSID must be 1-32 and password 8-63 characters");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Already connecting");
    }
    if (err != ESP_OK) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to connect");
    }

    return send_wifi_status(req, "202 Accepted");
}

/* Responds with [{"ssid": "...", "rssi": -52, "secure": true}, ...], strongest first.
 * Blocks for the length of the scan, during which the softAP pauses */
static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    wifi_station_scan_result_t results[WIFI_SCAN_MAX_RESULTS];
    size_t count;

    esp_err_t err = wifi_station_scan(results, WIFI_SCAN_MAX_RESULTS, &count);
    if (err == ESP_ERR_INVALID_STATE) {
        set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Busy connecting, try again shortly");
    }
    if (err != ESP_OK) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
    }

    cJSON *json = cJSON_CreateArray();
    for (size_t i = 0; json != NULL && i < count; i++) {
        cJSON *network = cJSON_CreateObject();

        /* once added, the object is freed together with the array */
        bool ok = network != NULL && cJSON_AddItemToArray(json, network);
        if (!ok) {
            cJSON_Delete(network);
        }

        ok = ok
            && cJSON_AddStringToObject(network, "ssid", results[i].ssid) != NULL
            && cJSON_AddNumberToObject(network, "rssi", results[i].rssi) != NULL
            && cJSON_AddBoolToObject(network, "secure", results[i].password_protected) != NULL;
        if (!ok) {
            cJSON_Delete(json);
            json = NULL;
        }
    }
    if (json == NULL) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    err = send_json(req, "200 OK", json);
    cJSON_Delete(json);
    return err;
}

/* Responds with {"trafiklab": true|false, "google": true|false}, true once a verified key is stored */
static esp_err_t send_api_keys_status(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL
        || cJSON_AddBoolToObject(json, "trafiklab", gtg_api_keys_is_set(GTG_API_KEY_TRAFIKLAB)) == NULL
        || cJSON_AddBoolToObject(json, "google", gtg_api_keys_is_set(GTG_API_KEY_GOOGLE)) == NULL) {
        cJSON_Delete(json);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    esp_err_t err = send_json(req, "200 OK", json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t api_keys_status_get_handler(httpd_req_t *req)
{
    return send_api_keys_status(req);
}

typedef struct {
    gtg_api_key_t key;
    const char *name;
    const char *rejected_msg;
} api_key_endpoint_t;

static const api_key_endpoint_t TRAFIKLAB_KEY_ENDPOINT = {
    .key          = GTG_API_KEY_TRAFIKLAB,
    .name         = "Trafiklab",
    .rejected_msg = "Trafiklab rejected the key, check that it is correct",
};

static const api_key_endpoint_t GOOGLE_KEY_ENDPOINT = {
    .key          = GTG_API_KEY_GOOGLE,
    .name         = "Google",
    .rejected_msg = "Google rejected the key, check that it is correct and that the Geocoding API is enabled for it",
};

/* Expects {"key": "..."}, verifies the key with a request to the service and stores it
 * when it works. Blocks for the length of that request. Responds like /api/keys/status */
static esp_err_t api_key_post_handler(httpd_req_t *req)
{
    const api_key_endpoint_t *endpoint = req->user_ctx;

    if (wifi_station_get_state(NULL) != WIFI_STATION_CONNECTED) {
        set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Connect to WiFi first");
    }

    esp_err_t err;
    cJSON *json = receive_json(req, &err);
    if (json == NULL) {
        return err;
    }

    char *key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "key"));
    err = key != NULL ? gtg_api_keys_set(endpoint->key, key) : ESP_ERR_INVALID_ARG;

    if (key != NULL) {
        memset(key, 0, strlen(key));
    }
    cJSON_Delete(json);

    if (err == ESP_ERR_INVALID_ARG) {
        _Static_assert(GTG_API_KEY_MAX_LEN == 64, "update the message below");
        return send_err(req, HTTPD_400_BAD_REQUEST,
                                   "The key must be 1-64 letters, digits, '-' or '_'");
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        set_status(req, "422 Unprocessable Content");
        return httpd_resp_sendstr(req, endpoint->rejected_msg);
    }
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Could not verify the key with %s, try again", endpoint->name);
        set_status(req, "502 Bad Gateway");
        return httpd_resp_sendstr(req, msg);
    }

    return send_api_keys_status(req);
}

/* Expects {"query": "..."} and responds with [{"id": "...", "name": "...", "town": "..."}, ...],
 * busiest first. Blocks for the length of the requests to Trafiklab and Google */
static esp_err_t stops_search_post_handler(httpd_req_t *req)
{
    if (wifi_station_get_state(NULL) != WIFI_STATION_CONNECTED) {
        set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Connect to WiFi first");
    }
    if (!gtg_api_keys_is_set(GTG_API_KEY_TRAFIKLAB) || !gtg_api_keys_is_set(GTG_API_KEY_GOOGLE)) {
        set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Verify both API keys first");
    }

    esp_err_t err;
    cJSON *json = receive_json(req, &err);
    if (json == NULL) {
        return err;
    }

    gtg_stop_t stops[GTG_STOP_SEARCH_MAX_RESULTS];
    size_t count = 0;

    const char *query = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "query"));
    err = query != NULL ? gtg_stop_search(query, stops, &count) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(json);

    if (err == ESP_ERR_INVALID_ARG) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "Enter a stop name of at most 64 characters");
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        set_status(req, "422 Unprocessable Content");
        return httpd_resp_sendstr(req, "Trafiklab rejected the key");
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        set_status(req, "422 Unprocessable Content");
        return httpd_resp_sendstr(req, "Too many stops match, try a more specific name");
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stop search failed: %s", esp_err_to_name(err));
        set_status(req, "502 Bad Gateway");
        return httpd_resp_sendstr(req, "Could not search Trafiklab, try again");
    }

    json = cJSON_CreateArray();
    for (size_t i = 0; json != NULL && i < count; i++) {
        cJSON *stop = cJSON_CreateObject();

        /* once added, the object is freed together with the array */
        bool ok = stop != NULL && cJSON_AddItemToArray(json, stop);
        if (!ok) {
            cJSON_Delete(stop);
        }

        ok = ok
            && cJSON_AddStringToObject(stop, "id", stops[i].id) != NULL
            && cJSON_AddStringToObject(stop, "name", stops[i].name) != NULL
            && cJSON_AddStringToObject(stop, "town", stops[i].town) != NULL;
        if (!ok) {
            cJSON_Delete(json);
            json = NULL;
        }
    }
    if (json == NULL) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    err = send_json(req, "200 OK", json);
    cJSON_Delete(json);
    return err;
}

/* Expects {"stop_id": "..."} and responds with
 * {"options": [{"line": "...", "mode": "...", "destination": "...", "tracked": bool}, ...], "samples": 8, "failed": n},
 * one option per unique departure during a whole day, tracked when it is in realtime_targets.
 * Blocks for the length of the requests to Trafiklab */
static esp_err_t departures_available_post_handler(httpd_req_t *req)
{
    if (wifi_station_get_state(NULL) != WIFI_STATION_CONNECTED) {
        set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Connect to WiFi first");
    }
    if (!gtg_api_keys_is_set(GTG_API_KEY_TRAFIKLAB)) {
        set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Verify the Trafiklab key first");
    }

    esp_err_t err;
    cJSON *json = receive_json(req, &err);
    if (json == NULL) {
        return err;
    }

    /* too large for the server task's stack next to the TLS connection */
    gtg_departure_option_t *options = malloc(GTG_DEPARTURES_MAX_OPTIONS * sizeof(gtg_departure_option_t));
    size_t count = 0;
    size_t failed = 0;

    /* kept past the request body for marking the tracked options */
    char stop_id[GTG_STOP_ID_MAX_LEN + 1] = "";
    const char *requested_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "stop_id"));

    if (options == NULL) {
        err = ESP_ERR_NO_MEM;
    } else if (requested_id == NULL || strlcpy(stop_id, requested_id, sizeof(stop_id)) >= sizeof(stop_id)) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        err = gtg_departures_list_options(stop_id, options, &count, &failed);
    }
    cJSON_Delete(json);

    if (err != ESP_OK) {
        free(options);
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "Invalid stop id");
    }
    if (err == ESP_ERR_NOT_FINISHED) {
        set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "The device clock isn't set yet, try again in a moment");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "Trafiklab doesn't know this stop, search again");
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        set_status(req, "422 Unprocessable Content");
        return httpd_resp_sendstr(req, "Trafiklab rejected the key");
    }
    if (err == ESP_ERR_NO_MEM) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "departure lookup failed: %s", esp_err_to_name(err));
        set_status(req, "502 Bad Gateway");
        return httpd_resp_sendstr(req, "Could not fetch departures from Trafiklab, try again");
    }

    json = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(json, "options");
    if (list == NULL
        || cJSON_AddNumberToObject(json, "samples", GTG_DEPARTURES_SAMPLES) == NULL
        || cJSON_AddNumberToObject(json, "failed", failed) == NULL) {
        cJSON_Delete(json);
        json = NULL;
    }

    for (size_t i = 0; json != NULL && i < count; i++) {
        cJSON *option = cJSON_CreateObject();

        /* once added, the object is freed together with the response */
        bool ok = option != NULL && cJSON_AddItemToArray(list, option);
        if (!ok) {
            cJSON_Delete(option);
        }

        ok = ok
            && cJSON_AddStringToObject(option, "line", options[i].line) != NULL
            && cJSON_AddStringToObject(option, "mode", options[i].mode) != NULL
            && cJSON_AddStringToObject(option, "destination", options[i].destination) != NULL
            && cJSON_AddBoolToObject(option, "tracked", gtg_dev_config_has_target(stop_id, &options[i])) != NULL;
        if (!ok) {
            cJSON_Delete(json);
            json = NULL;
        }
    }
    free(options);

    if (json == NULL) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    err = send_json(req, "200 OK", json);
    cJSON_Delete(json);
    return err;
}

/* Copies a string member into a fixed field. False when it is missing, too long or holds control
 * characters, or when it is empty and required */
static bool copy_json_string(const cJSON *object, const char *name, char *field, size_t field_size, bool required)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, name));
    if (value == NULL || (required && value[0] == '\0')) {
        return false;
    }

    for (const char *c = value; *c != '\0'; c++) {
        if ((unsigned char)*c < 0x20 || *c == 0x7f) {
            return false;
        }
    }

    return strlcpy(field, value, field_size) < field_size;
}

static bool is_digits(const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
    }
    return true;
}

/* Adds {"id", "name", "town"} of the stop and {"line", "mode", "destination"} of the departure */
static bool add_target_json(cJSON *list, const trafiklab_target_t *target)
{
    cJSON *item = cJSON_CreateObject();

    /* once added, the object is freed together with the list */
    if (item == NULL || !cJSON_AddItemToArray(list, item)) {
        cJSON_Delete(item);
        return false;
    }

    cJSON *stop = cJSON_AddObjectToObject(item, "stop");
    cJSON *departure = cJSON_AddObjectToObject(item, "departure");

    return stop != NULL && departure != NULL
        && cJSON_AddStringToObject(stop, "id", target->stop.id) != NULL
        && cJSON_AddStringToObject(stop, "name", target->stop.name) != NULL
        && cJSON_AddStringToObject(stop, "town", target->stop.town) != NULL
        && cJSON_AddStringToObject(departure, "line", target->departure.line) != NULL
        && cJSON_AddStringToObject(departure, "mode", target->departure.mode) != NULL
        && cJSON_AddStringToObject(departure, "destination", target->departure.destination) != NULL;
}

/* Responds with {"targets": [{"stop": {...}, "departure": {...}}, ...], "max": 8}, in the order added */
static esp_err_t send_targets(httpd_req_t *req, const char *status)
{
    trafiklab_target_t targets[GTG_DEV_CONFIG_MAX_TARGETS];
    size_t count = 0;
    if (gtg_dev_config_get_targets(targets, GTG_DEV_CONFIG_MAX_TARGETS, &count) != ESP_OK) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not read the tracked departures");
    }

    cJSON *json = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(json, "targets");
    bool ok = list != NULL && cJSON_AddNumberToObject(json, "max", GTG_DEV_CONFIG_MAX_TARGETS) != NULL;

    for (size_t i = 0; ok && i < count; i++) {
        ok = add_target_json(list, &targets[i]);
    }

    if (!ok) {
        cJSON_Delete(json);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }

    esp_err_t err = send_json(req, status, json);
    cJSON_Delete(json);
    return err;
}

static esp_err_t targets_get_handler(httpd_req_t *req)
{
    return send_targets(req, "200 OK");
}

/* Expects {"stop": {"id": "...", "name": "...", "town": "..."},
 *          "departure": {"line": "...", "mode": "...", "destination": "..."}}
 * as returned by /api/stops/search and /api/departures/available, adds it to realtime_targets
 * and responds like GET /api/targets */
static esp_err_t targets_post_handler(httpd_req_t *req)
{
    esp_err_t err;
    cJSON *json = receive_json(req, &err);
    if (json == NULL) {
        return err;
    }

    const cJSON *stop = cJSON_GetObjectItemCaseSensitive(json, "stop");
    const cJSON *departure = cJSON_GetObjectItemCaseSensitive(json, "departure");

    trafiklab_target_t target = { 0 };
    bool valid = copy_json_string(stop, "id", target.stop.id, sizeof(target.stop.id), true)
        && is_digits(target.stop.id)
        && copy_json_string(stop, "name", target.stop.name, sizeof(target.stop.name), true)
        && copy_json_string(stop, "town", target.stop.town, sizeof(target.stop.town), false)
        && copy_json_string(departure, "line", target.departure.line, sizeof(target.departure.line), false)
        && copy_json_string(departure, "mode", target.departure.mode, sizeof(target.departure.mode), true)
        && copy_json_string(departure, "destination", target.departure.destination,
                            sizeof(target.departure.destination), true);
    cJSON_Delete(json);

    if (!valid) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "Invalid stop or departure");
    }

    err = gtg_dev_config_add_target(&target, NULL);

    if (err == ESP_ERR_NOT_ALLOWED) {
        set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "This departure is already tracked");
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        char msg[48];
        snprintf(msg, sizeof(msg), "You can track at most %d departures", GTG_DEV_CONFIG_MAX_TARGETS);
        set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, msg);
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "Invalid stop or departure");
    }
    if (err != ESP_OK) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not add the departure");
    }

    return send_targets(req, "201 Created");
}

/* Expects {"stop_id": "...", "departure": {"line": "...", "mode": "...", "destination": "..."}},
 * removes that target from realtime_targets and responds like GET /api/targets. The target is
 * named by its content rather than its place, which can change from another tab */
static esp_err_t targets_delete_handler(httpd_req_t *req)
{
    esp_err_t err;
    cJSON *json = receive_json(req, &err);
    if (json == NULL) {
        return err;
    }

    const cJSON *departure = cJSON_GetObjectItemCaseSensitive(json, "departure");

    char stop_id[GTG_STOP_ID_MAX_LEN + 1] = "";
    gtg_departure_option_t option = { 0 };
    bool valid = copy_json_string(json, "stop_id", stop_id, sizeof(stop_id), true)
        && copy_json_string(departure, "line", option.line, sizeof(option.line), false)
        && copy_json_string(departure, "mode", option.mode, sizeof(option.mode), true)
        && copy_json_string(departure, "destination", option.destination, sizeof(option.destination), true);
    cJSON_Delete(json);

    if (!valid) {
        return send_err(req, HTTPD_400_BAD_REQUEST, "Invalid stop or departure");
    }

    err = gtg_dev_config_remove_target(stop_id, &option);

    if (err == ESP_ERR_NOT_FOUND) {
        set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "This departure is no longer tracked");
    }
    if (err != ESP_OK) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not remove the departure");
    }

    return send_targets(req, "200 OK");
}

static void restart_cb(void *arg)
{
    esp_restart();
}

/* Expects {"confirm": "factory-reset"}, erases the stored configuration and restarts the device,
 * which brings every module back to its first boot state. The confirmation guards against an
 * accidental request from a logged in page */
static esp_err_t factory_reset_post_handler(httpd_req_t *req)
{
    esp_err_t err;
    cJSON *json = receive_json(req, &err);
    if (json == NULL) {
        return err;
    }

    const char *confirm = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "confirm"));
    bool confirmed = confirm != NULL && strcmp(confirm, FACTORY_RESET_CONFIRM) == 0;
    cJSON_Delete(json);

    if (!confirmed) {
        return send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Confirm with {\"confirm\": \"" FACTORY_RESET_CONFIRM "\"}");
    }

    esp_timer_handle_t timer = NULL;
    const esp_timer_create_args_t timer_args = {
        .callback = restart_cb,
        .name     = "factory reset",
    };

    /* the timer is made first, so the configuration isn't erased on a device that can't restart */
    if (esp_timer_create(&timer_args, &timer) != ESP_OK) {
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not prepare the restart");
    }

    err = gtg_dev_config_factory_reset();
    if (err != ESP_OK) {
        esp_timer_delete(timer);
        return send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not erase the configuration");
    }

    set_status(req, "202 Accepted");
    err = httpd_resp_sendstr(req, "Factory reset done, the device restarts");

    /* restarts also when the response couldn't be sent, the configuration is already gone */
    if (esp_timer_start_once(timer, FACTORY_RESET_RESTART_DELAY_US) != ESP_OK) {
        esp_restart();
    }

    return err;
}

/* Compares in a time that doesn't depend on where the strings differ, so the response time tells
 * nothing about the secret */
static bool equal_const_time(const char *a, const char *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/* A new random password of letters and digits, with at least one of each */
static void generate_password(void)
{
    _Static_assert(sizeof(PASSWORD_ALPHABET) - 1 == 32, "the alphabet must divide 256 for an even pick");

    bool letter;
    bool digit;
    do {
        uint8_t random[GTG_HTTPS_PASSWORD_LEN];
        esp_fill_random(random, sizeof(random));

        letter = false;
        digit = false;
        for (size_t i = 0; i < GTG_HTTPS_PASSWORD_LEN; i++) {
            char c = PASSWORD_ALPHABET[random[i] % 32];
            s_password[i] = c;
            letter |= c >= 'A' && c <= 'Z';
            digit |= c >= '0' && c <= '9';
        }
        s_password[GTG_HTTPS_PASSWORD_LEN] = '\0';
    } while (!letter || !digit);
}

/* The password is shown in capitals, a phone keyboard may still send small letters */
static bool password_matches(const char *given)
{
    if (strlen(given) != GTG_HTTPS_PASSWORD_LEN) {
        return false;
    }

    char upper[GTG_HTTPS_PASSWORD_LEN];
    for (size_t i = 0; i < GTG_HTTPS_PASSWORD_LEN; i++) {
        char c = given[i];
        upper[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }

    bool match = equal_const_time(upper, s_password, GTG_HTTPS_PASSWORD_LEN);
    memset(upper, 0, sizeof(upper));
    return match;
}

static void clear_sessions(void)
{
    memset(s_sessions, 0, sizeof(s_sessions));
}

/* Starts a session in a free slot, or in place of the least recently used one */
static const session_t *session_create(int64_t now)
{
    session_t *slot = &s_sessions[0];
    for (size_t i = 0; i < SESSION_MAX; i++) {
        if (s_sessions[i].token[0] == '\0') {
            slot = &s_sessions[i];
            break;
        }
        if (s_sessions[i].last_used_us < slot->last_used_us) {
            slot = &s_sessions[i];
        }
    }

    uint8_t random[SESSION_TOKEN_BYTES];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < SESSION_TOKEN_BYTES; i++) {
        snprintf(&slot->token[2 * i], 3, "%02x", random[i]);
    }
    memset(random, 0, sizeof(random));

    slot->last_used_us = now;
    return slot;
}

/* The live session whose cookie the request carries, NULL when there is none. It isn't renewed
 * here, only once the request has succeeded, see tracked_handler(). Sessions that timed out are
 * ended on the way */
static session_t *session_find(httpd_req_t *req, int64_t now)
{
    char token[SESSION_TOKEN_LEN + 1];
    size_t token_size = sizeof(token);

    bool found = httpd_req_get_cookie_val(req, SESSION_COOKIE, token, &token_size) == ESP_OK
              && strlen(token) == SESSION_TOKEN_LEN;

    session_t *match = NULL;
    for (size_t i = 0; i < SESSION_MAX; i++) {
        session_t *session = &s_sessions[i];
        if (session->token[0] == '\0') {
            continue;
        }
        if (now - session->last_used_us >= SESSION_TIMEOUT_US) {
            ESP_LOGI(TAG, "session timed out");
            memset(session, 0, sizeof(*session));
            continue;
        }
        if (found && equal_const_time(token, session->token, SESSION_TOKEN_LEN)) {
            match = session;
        }
    }

    memset(token, 0, sizeof(token));
    return match;
}

/* Expects {"password": "..."}, the one on the device's LCD, and starts a session in a cookie.
 * 401 for a wrong password, 429 while the login is locked after LOGIN_MAX_FAILURES in a row */
static esp_err_t login_post_handler(httpd_req_t *req)
{
    int64_t now = esp_timer_get_time();

    if (now < s_login_locked_until_us) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Too many wrong passwords, try again in %d seconds",
                 (int)((s_login_locked_until_us - now) / 1000000) + 1);
        set_status(req, "429 Too Many Requests");
        return httpd_resp_sendstr(req, msg);
    }

    esp_err_t err;
    cJSON *json = receive_json(req, &err);
    if (json == NULL) {
        return err;
    }

    char *password = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "password"));
    bool match = password != NULL && password_matches(password);

    if (password != NULL) {
        memset(password, 0, strlen(password));
    }
    cJSON_Delete(json);

    if (!match) {
        s_login_failures++;
        ESP_LOGW(TAG, "wrong login password (%u/%d)", s_login_failures, LOGIN_MAX_FAILURES);

        if (s_login_failures >= LOGIN_MAX_FAILURES) {
            s_login_failures = 0;
            s_login_locked_until_us = now + LOGIN_LOCK_US;
            ESP_LOGW(TAG, "login locked for %lld s", LOGIN_LOCK_US / 1000000);
        }

        set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "Wrong password, check the device's screen");
    }

    s_login_failures = 0;
    const session_t *session = session_create(now);

    /* no Max-Age, the browser keeps it until it closes and the device ends it after 5 idle minutes */
    char cookie[sizeof(SESSION_COOKIE) + SESSION_TOKEN_LEN + 64];
    snprintf(cookie, sizeof(cookie), SESSION_COOKIE "=%s; Path=/; Secure; HttpOnly; SameSite=Strict",
             session->token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    s_last_activity_us = now;
    ESP_LOGI(TAG, "logged in");

    gtg_event_t event = { .type = GTG_EV_CONFIG_LOGIN };
    gtg_events_post(&event);

    /* the header points at cookie, which must outlive the send */
    err = httpd_resp_sendstr(req, "Logged in");
    memset(cookie, 0, sizeof(cookie));
    return err;
}

static const httpd_uri_t api_uris[] = {
    {
        .uri       = "/api/login",
        .method    = HTTP_POST,
        .handler   = login_post_handler,
    },
    {
        .uri       = "/api/status",
        .method    = HTTP_GET,
        .handler   = status_get_handler,
    },
    {
        .uri       = "/api/wifi/status",
        .method    = HTTP_GET,
        .handler   = wifi_status_get_handler,
    },
    {
        .uri       = "/api/wifi/scan",
        .method    = HTTP_GET,
        .handler   = wifi_scan_get_handler,
    },
    {
        .uri       = "/api/wifi/connect",
        .method    = HTTP_POST,
        .handler   = wifi_connect_post_handler,
    },
    {
        .uri       = "/api/keys/status",
        .method    = HTTP_GET,
        .handler   = api_keys_status_get_handler,
    },
    {
        .uri       = "/api/keys/trafiklab",
        .method    = HTTP_POST,
        .handler   = api_key_post_handler,
        .user_ctx  = (void *)&TRAFIKLAB_KEY_ENDPOINT,
    },
    {
        .uri       = "/api/keys/google",
        .method    = HTTP_POST,
        .handler   = api_key_post_handler,
        .user_ctx  = (void *)&GOOGLE_KEY_ENDPOINT,
    },
    {
        .uri       = "/api/stops/search",
        .method    = HTTP_POST,
        .handler   = stops_search_post_handler,
    },
    {
        .uri       = "/api/departures/available",
        .method    = HTTP_POST,
        .handler   = departures_available_post_handler,
    },
    {
        .uri       = "/api/targets",
        .method    = HTTP_GET,
        .handler   = targets_get_handler,
    },
    {
        .uri       = "/api/targets",
        .method    = HTTP_POST,
        .handler   = targets_post_handler,
    },
    {
        .uri       = "/api/targets",
        .method    = HTTP_DELETE,
        .handler   = targets_delete_handler,
    },
    {
        .uri       = "/api/factory-reset",
        .method    = HTTP_POST,
        .handler   = factory_reset_post_handler,
    },
    {
        .uri       = "/api/config/apply",
        .method    = HTTP_POST,
        .handler   = config_apply_post_handler,
    },
    /* Must stay last. URI handlers are matched in registration order, so the API endpoints
     * come before this catch-all */
    {
        .uri       = "/*",
        .method    = HTTP_GET,
        .handler   = static_file_get_handler,
    },
};

#define URI_COUNT   (sizeof(api_uris) / sizeof(api_uris[0]))

/* The login itself, and the static files, which hold no configuration: the page shows the login
 * form when the API answers 401 */
static bool is_open(const httpd_uri_t *uri)
{
    return uri->handler == login_post_handler || uri->handler == static_file_get_handler;
}

/* Every URI is registered with this handler, which checks the session of every request but the
 * open ones and calls the URI's own handler with its own user_ctx. Only a request that succeeded,
 * with a 2xx response, renews its session and counts as activity, so failing requests can't keep
 * a session or the server alive */
static esp_err_t tracked_handler(httpd_req_t *req)
{
    const httpd_uri_t *uri = req->user_ctx;
    req->user_ctx = uri->user_ctx;

    if (is_open(uri)) {
        return uri->handler(req);
    }

    session_t *session = session_find(req, esp_timer_get_time());
    if (session == NULL) {
        set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, "Log in first");
    }

    /* the server answers 200 OK unless the handler sets another status */
    s_resp_success = true;
    esp_err_t err = uri->handler(req);

    /* from when it ended, as e.g. the departure search can take a minute */
    if (err == ESP_OK && s_resp_success) {
        int64_t done = esp_timer_get_time();
        session->last_used_us = done;
        s_last_activity_us = done;
    }

    return err;
}

esp_err_t gtg_https_server_get_password(char *out, size_t out_size)
{
    if (out == NULL || out_size < sizeof(s_password)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(out, s_password, sizeof(s_password));
    return ESP_OK;
}

int64_t gtg_https_server_last_activity_us(void)
{
    return s_last_activity_us;
}

bool gtg_https_server_is_running(void)
{
    return s_server != NULL;
}

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

    /* a new password and no sessions, so a login from an earlier configuration mode is over */
    generate_password();
    clear_sessions();
    s_login_failures = 0;
    s_login_locked_until_us = 0;

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.uri_match_fn = httpd_uri_match_wildcard;
    conf.httpd.max_uri_handlers = URI_COUNT;

    /* the API key and stop search handlers make their own TLS connections from the server task */
    conf.httpd.stack_size = HTTPD_STACK_SIZE;

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

    for (size_t i = 0; i < URI_COUNT && err == ESP_OK; i++) {
        /* registering copies the struct, the user_ctx points back at the URI's entry */
        httpd_uri_t tracked = api_uris[i];
        tracked.handler = tracked_handler;
        tracked.user_ctx = (void *)&api_uris[i];
        err = httpd_register_uri_handler(s_server, &tracked);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register URI handlers");
        gtg_https_server_deinit();
        return err;
    }

    /* the idle time counts from the start until the first request */
    s_last_activity_us = esp_timer_get_time();

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

    clear_sessions();
    memset(s_password, 0, sizeof(s_password));

    www_unmount();
}
