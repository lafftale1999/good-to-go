#include "gtg_stop_search.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "gtg_api_keys.h"

#define API_REQUEST_TIMEOUT_MS  10000

/* The encoded query can be three times its length, which doesn't fit the default 512 bytes */
#define HTTP_TX_BUFFER_SIZE     1024

#define TRAFIKLAB_SEARCH_URL    "https://realtime-api.trafiklab.se/v1/stops/name/%s?key=%s"
#define TRAFIKLAB_URL_MAX_LEN   (sizeof(TRAFIKLAB_SEARCH_URL) + GTG_STOP_QUERY_MAX_LEN * 3 + GTG_API_KEY_MAX_LEN)

/* Around 400 bytes per stop group, a name that matches more than this many is too vague to list */
#define TRAFIKLAB_BODY_MAX_LEN  16384

/* result_type=postal_town leaves a single result of around 2 KB */
#define GOOGLE_RESOLVE_URL      "https://maps.googleapis.com/maps/api/geocode/json?latlng=%.6f,%.6f&result_type=postal_town&language=sv&key=%s"
#define GOOGLE_URL_MAX_LEN      (sizeof(GOOGLE_RESOLVE_URL) + 2 * 16 + GTG_API_KEY_MAX_LEN)
#define GOOGLE_BODY_MAX_LEN     4096

static const char *TAG = "stop search";

typedef struct {
    char   *data;
    size_t  size;       /* capacity including the NUL terminator */
    size_t  len;
    bool    overflow;   /* the response was larger than the buffer */
} response_body_t;

typedef struct {
    gtg_stop_t stop;
    double     stop_times;
    double     lat;
    double     lon;
    bool       has_location;
} candidate_t;

/* Collects the response body into the response_body_t set as user data */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    response_body_t *body = evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && body != NULL) {
        size_t len = evt->data_len;
        size_t space = body->size - 1 - body->len;
        if (len > space) {
            len = space;
            body->overflow = true;
        }

        memcpy(body->data + body->len, evt->data, len);
        body->len += len;
        body->data[body->len] = '\0';
    }

    return ESP_OK;
}

static esp_http_client_handle_t http_client_create(const char *url, response_body_t *body)
{
    esp_http_client_config_t config = {
        .url               = url,
        .timeout_ms        = API_REQUEST_TIMEOUT_MS,
        .buffer_size_tx    = HTTP_TX_BUFFER_SIZE,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event_handler,
        .user_data         = body,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != NULL && esp_http_client_set_header(client, "Accept", "application/json") != ESP_OK) {
        esp_http_client_cleanup(client);
        client = NULL;
    }

    return client;
}

/* Sends a GET request with the client. Requests to the same host reuse the open connection,
 * so only the first one makes a TLS handshake */
static esp_err_t http_get(esp_http_client_handle_t client, const char *url, response_body_t *body, int *status)
{
    body->len = 0;
    body->overflow = false;
    body->data[0] = '\0';

    ESP_RETURN_ON_ERROR(esp_http_client_set_url(client, url), TAG, "invalid URL");
    ESP_RETURN_ON_ERROR(esp_http_client_perform(client), TAG, "request failed");

    *status = esp_http_client_get_status_code(client);

    return body->overflow ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static bool is_valid_query(const char *query)
{
    size_t len = strnlen(query, GTG_STOP_QUERY_MAX_LEN + 1);
    if (len == 0 || len > GTG_STOP_QUERY_MAX_LEN) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)query[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }

    return true;
}

/* Percent-encodes everything but the unreserved characters of RFC 3986 */
static bool url_encode(const char *in, char *out, size_t out_size)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t len = 0;

    for (; *in != '\0'; in++) {
        unsigned char c = (unsigned char)*in;

        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            if (len + 1 >= out_size) {
                return false;
            }
            out[len++] = (char)c;
        } else {
            if (len + 3 >= out_size) {
                return false;
            }
            out[len++] = '%';
            out[len++] = HEX[c >> 4];
            out[len++] = HEX[c & 0x0f];
        }
    }

    out[len] = '\0';
    return true;
}

/* Copies a UTF-8 string, cutting a too long one before the character that doesn't fit */
static void copy_utf8(char *dst, size_t dst_size, const char *src)
{
    if (strlcpy(dst, src, dst_size) < dst_size) {
        return;
    }

    /* src[end] is the first byte left out, step back while it continues a character */
    size_t end = dst_size - 1;
    while (end > 0 && ((unsigned char)src[end] & 0xc0) == 0x80) {
        end--;
    }
    dst[end] = '\0';
}

/* Inserts the candidate into the list sorted by stop times, busiest first, keeping at most
 * GTG_STOP_SEARCH_MAX_RESULTS. Equal stop times keep Trafiklab's order */
static void insert_candidate(candidate_t *list, size_t *count, const candidate_t *candidate)
{
    size_t pos = *count;
    while (pos > 0 && list[pos - 1].stop_times < candidate->stop_times) {
        pos--;
    }
    if (pos >= GTG_STOP_SEARCH_MAX_RESULTS) {
        return;
    }

    /* the last one falls off a full list */
    size_t moved = (*count < GTG_STOP_SEARCH_MAX_RESULTS ? *count : GTG_STOP_SEARCH_MAX_RESULTS - 1) - pos;
    memmove(&list[pos + 1], &list[pos], moved * sizeof(list[0]));
    list[pos] = *candidate;

    if (*count < GTG_STOP_SEARCH_MAX_RESULTS) {
        (*count)++;
    }
}

/* Reads a stop group, false when it should be skipped */
static bool parse_stop_group(const cJSON *group, candidate_t *candidate)
{
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(group, "id"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(group, "name"));
    const cJSON *stop_times = cJSON_GetObjectItemCaseSensitive(group, "average_daily_stop_times");
    const cJSON *modes = cJSON_GetObjectItemCaseSensitive(group, "transport_modes");

    if (id == NULL || name == NULL || !cJSON_IsNumber(stop_times) || !cJSON_IsArray(modes)) {
        return false;
    }
    if (cJSON_GetArraySize(modes) == 0 || stop_times->valuedouble <= 0) {
        return false;
    }

    size_t id_len = strlen(id);
    if (id_len == 0 || id_len > GTG_STOP_ID_MAX_LEN) {
        ESP_LOGW(TAG, "skipping stop group with unexpected id \"%s\"", id);
        return false;
    }

    memset(candidate, 0, sizeof(*candidate));
    strlcpy(candidate->stop.id, id, sizeof(candidate->stop.id));
    copy_utf8(candidate->stop.name, sizeof(candidate->stop.name), name);
    candidate->stop_times = stop_times->valuedouble;

    /* the stops of a group are at the same place, the first one locates it */
    const cJSON *stop = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(group, "stops"), 0);
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(stop, "lat");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(stop, "lon");
    if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
        candidate->lat = lat->valuedouble;
        candidate->lon = lon->valuedouble;
        candidate->has_location = true;
    }

    return true;
}

static esp_err_t parse_stop_groups(const char *body, candidate_t *candidates, size_t *count)
{
    cJSON *json = cJSON_Parse(body);
    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(json, "stop_groups");

    if (!cJSON_IsArray(groups)) {
        ESP_LOGE(TAG, "Trafiklab response has no stop_groups");
        cJSON_Delete(json);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t total = 0;
    const cJSON *group;
    cJSON_ArrayForEach(group, groups) {
        candidate_t candidate;
        if (parse_stop_group(group, &candidate)) {
            insert_candidate(candidates, count, &candidate);
        }
        total++;
    }

    ESP_LOGI(TAG, "Trafiklab found %u stop groups, %u kept", (unsigned)total, (unsigned)*count);

    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t search_trafiklab(const char *query, candidate_t *candidates, size_t *count)
{
    char key[GTG_API_KEY_MAX_LEN + 1];
    if (gtg_api_keys_get(GTG_API_KEY_TRAFIKLAB, key, sizeof(key)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    char encoded[GTG_STOP_QUERY_MAX_LEN * 3 + 1];
    if (!url_encode(query, encoded, sizeof(encoded))) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[TRAFIKLAB_URL_MAX_LEN];
    snprintf(url, sizeof(url), TRAFIKLAB_SEARCH_URL, encoded, key);
    memset(key, 0, sizeof(key));

    response_body_t body = {
        .data = malloc(TRAFIKLAB_BODY_MAX_LEN),
        .size = TRAFIKLAB_BODY_MAX_LEN,
    };
    if (body.data == NULL) {
        memset(url, 0, sizeof(url));
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_handle_t client = http_client_create(url, &body);
    if (client == NULL) {
        memset(url, 0, sizeof(url));
        free(body.data);
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = http_get(client, url, &body, &status);
    esp_http_client_cleanup(client);
    memset(url, 0, sizeof(url));

    if (err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGW(TAG, "Trafiklab response is larger than %d bytes", TRAFIKLAB_BODY_MAX_LEN);
    } else if (err == ESP_OK) {
        ESP_LOGI(TAG, "Trafiklab responded with %d, %u bytes", status, (unsigned)body.len);

        if (status == 404) {
            /* no stop matches the name */
        } else if (status == 401 || status == 403) {
            err = ESP_ERR_NOT_ALLOWED;
        } else if (status != 200) {
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            err = parse_stop_groups(body.data, candidates, count);
        }
    }

    free(body.data);
    return err;
}

/* Finds the postal_town component of the first result. A missing town leaves town empty */
static esp_err_t parse_postal_town(const char *body, char *town, size_t town_size)
{
    cJSON *json = cJSON_Parse(body);
    const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "status"));

    esp_err_t err;
    if (status == NULL) {
        err = ESP_ERR_INVALID_RESPONSE;
    } else if (strcmp(status, "OK") == 0 || strcmp(status, "ZERO_RESULTS") == 0) {
        err = ESP_OK;
    } else if (strcmp(status, "REQUEST_DENIED") == 0 || strcmp(status, "OVER_QUERY_LIMIT") == 0) {
        err = ESP_ERR_NOT_ALLOWED;
    } else {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    if (err != ESP_OK) {
        const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "error_message"));
        ESP_LOGW(TAG, "Google responded with %s%s%s", status != NULL ? status : "an invalid body",
                 message != NULL ? ": " : "", message != NULL ? message : "");
        cJSON_Delete(json);
        return err;
    }

    const cJSON *result = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "results"), 0);
    const cJSON *component;
    cJSON_ArrayForEach(component, cJSON_GetObjectItemCaseSensitive(result, "address_components")) {
        const char *long_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(component, "long_name"));
        const cJSON *type;
        cJSON_ArrayForEach(type, cJSON_GetObjectItemCaseSensitive(component, "types")) {
            const char *type_name = cJSON_GetStringValue(type);
            if (long_name != NULL && type_name != NULL && strcmp(type_name, "postal_town") == 0) {
                copy_utf8(town, town_size, long_name);
                cJSON_Delete(json);
                return ESP_OK;
            }
        }
    }

    cJSON_Delete(json);
    return ESP_OK;
}

/* Resolves the towns over one connection. Failed lookups leave the town empty, and lookups
 * stop early when Google rejects the key or can't be reached, as the rest would fail too */
static void resolve_towns(candidate_t *candidates, size_t count)
{
    char key[GTG_API_KEY_MAX_LEN + 1];
    if (gtg_api_keys_get(GTG_API_KEY_GOOGLE, key, sizeof(key)) != ESP_OK) {
        ESP_LOGW(TAG, "no Google key, towns are not resolved");
        return;
    }

    response_body_t body = {
        .data = malloc(GOOGLE_BODY_MAX_LEN),
        .size = GOOGLE_BODY_MAX_LEN,
    };
    esp_http_client_handle_t client = NULL;

    char url[GOOGLE_URL_MAX_LEN];
    for (size_t i = 0; i < count && body.data != NULL; i++) {
        if (!candidates[i].has_location) {
            continue;
        }

        snprintf(url, sizeof(url), GOOGLE_RESOLVE_URL, candidates[i].lat, candidates[i].lon, key);

        if (client == NULL) {
            client = http_client_create(url, &body);
            if (client == NULL) {
                break;
            }
        }

        int status = 0;
        esp_err_t err = http_get(client, url, &body, &status);
        if (err == ESP_OK) {
            err = status == 200 ? parse_postal_town(body.data, candidates[i].stop.town, sizeof(candidates[i].stop.town))
                                : ESP_ERR_INVALID_RESPONSE;
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "could not resolve the town of %s (status %d): %s",
                     candidates[i].stop.id, status, esp_err_to_name(err));
        }
        if (err == ESP_ERR_NOT_ALLOWED || (err != ESP_OK && status == 0)) {
            break;
        }
    }

    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    memset(url, 0, sizeof(url));
    memset(key, 0, sizeof(key));
    free(body.data);
}

esp_err_t gtg_stop_search(const char *query, gtg_stop_t *results, size_t *count)
{
    if (query == NULL || results == NULL || count == NULL || !is_valid_query(query)) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;

    candidate_t *candidates = calloc(GTG_STOP_SEARCH_MAX_RESULTS, sizeof(candidate_t));
    if (candidates == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t found = 0;
    esp_err_t err = search_trafiklab(query, candidates, &found);
    if (err == ESP_OK) {
        resolve_towns(candidates, found);

        for (size_t i = 0; i < found; i++) {
            results[i] = candidates[i].stop;
        }
        *count = found;
    }

    free(candidates);
    return err;
}
