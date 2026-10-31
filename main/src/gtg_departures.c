#include "gtg_departures.h"

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
#include "gtg_str.h"
#include "gtg_stop_search.h"

#define API_REQUEST_TIMEOUT_MS  10000

#define DEPARTURES_URL          "https://realtime-api.trafiklab.se/v1/departures/%s?key=%s"
#define DEPARTURES_URL_MAX_LEN  (sizeof(DEPARTURES_URL) + GTG_STOP_ID_MAX_LEN + GTG_API_KEY_MAX_LEN)

/* One departure is around 1.2 KB, more with alerts. A larger one is skipped */
#define DEPARTURE_JSON_MAX_LEN  8192

/* The array of the response that holds the departures, the only part that is parsed */
#define DEPARTURES_KEY          "departures"

static const char *TAG = "departures";

/*
 * Splits the response into its departures while it is received. Only the nesting, strings and
 * the top-level key in front of each array are tracked, so a response of any size goes through a
 * buffer of one departure:
 *
 *   {                          depth 1, top-level keys are remembered
 *     "stops": [ ... ],
 *     "departures": [          depth 2 once "departures" opens
 *       { ... },               depth 3, each object is collected and parsed on its own
 *     ]
 *   }
 */
typedef struct {
    int    depth;
    bool   in_string;
    bool   escape;
    bool   in_departures;

    char   key[sizeof(DEPARTURES_KEY) + 1];    /* last string at depth 1, cut when longer */
    size_t key_len;

    char  *departure;           /* the departure being collected */
    size_t departure_len;
    bool   collecting;
    bool   too_long;

    gtg_departure_option_t *options;
    size_t                  count;
    size_t                  departures;     /* departures seen */
    size_t                  skipped;        /* departures too long or not understood */
    size_t                  dropped;        /* unique options that didn't fit */
} departure_stream_t;

static bool is_valid_stop_id(const char *id)
{
    size_t len = strnlen(id, GTG_STOP_ID_MAX_LEN + 1);
    if (len == 0 || len > GTG_STOP_ID_MAX_LEN) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)id[i])) {
            return false;
        }
    }

    return true;
}

static bool same_option(const gtg_departure_option_t *a, const gtg_departure_option_t *b)
{
    return strcmp(a->line, b->line) == 0
        && strcmp(a->mode, b->mode) == 0
        && strcmp(a->destination, b->destination) == 0;
}

static void add_option(departure_stream_t *stream, const gtg_departure_option_t *option)
{
    for (size_t i = 0; i < stream->count; i++) {
        if (same_option(&stream->options[i], option)) {
            return;
        }
    }

    if (stream->count < GTG_DEPARTURES_MAX_OPTIONS) {
        stream->options[stream->count++] = *option;
    } else {
        stream->dropped++;
    }
}

/* Reads the route of one departure into an option */
static void parse_departure(departure_stream_t *stream)
{
    cJSON *json = cJSON_ParseWithLength(stream->departure, stream->departure_len);
    const cJSON *route = cJSON_GetObjectItemCaseSensitive(json, "route");

    /* named lines have no designation, their name tells them apart */
    const char *line = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(route, "designation"));
    if (line == NULL) {
        line = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(route, "name"));
    }
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(route, "transport_mode"));
    const char *destination = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(route, "destination"), "name"));

    if (mode == NULL || destination == NULL) {
        stream->skipped++;
    } else {
        gtg_departure_option_t option;
        gtg_copy_utf8(option.line, sizeof(option.line), line != NULL ? line : "");
        gtg_copy_utf8(option.mode, sizeof(option.mode), mode);
        gtg_copy_utf8(option.destination, sizeof(option.destination), destination);
        add_option(stream, &option);
    }

    cJSON_Delete(json);
}

static void collect(departure_stream_t *stream, char c)
{
    if (stream->departure_len < DEPARTURE_JSON_MAX_LEN) {
        stream->departure[stream->departure_len++] = c;
    } else {
        stream->too_long = true;
    }
}

static void stream_feed(departure_stream_t *stream, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];

        if (stream->collecting) {
            collect(stream, c);
        }

        if (stream->in_string) {
            if (stream->escape) {
                stream->escape = false;
            } else if (c == '\\') {
                stream->escape = true;
            } else if (c == '"') {
                stream->in_string = false;
            } else if (stream->depth == 1 && stream->key_len < sizeof(stream->key) - 1) {
                stream->key[stream->key_len++] = c;
            }
            continue;
        }

        switch (c) {
        case '"':
            stream->in_string = true;
            if (stream->depth == 1) {
                stream->key_len = 0;
            }
            break;

        case '[':
        case '{':
            if (c == '[' && stream->depth == 1) {
                /* the string before an array at depth 1 is always its key */
                stream->key[stream->key_len] = '\0';
                stream->in_departures = strcmp(stream->key, DEPARTURES_KEY) == 0;
            }
            if (c == '{' && stream->depth == 2 && stream->in_departures) {
                stream->collecting = true;
                stream->too_long = false;
                stream->departure_len = 0;
                collect(stream, c);
            }
            stream->depth++;
            break;

        case ']':
        case '}':
            stream->depth--;
            if (c == '}' && stream->depth == 2 && stream->collecting) {
                stream->collecting = false;
                stream->departures++;
                if (stream->too_long) {
                    stream->skipped++;
                } else {
                    parse_departure(stream);
                }
            }
            if (c == ']' && stream->depth == 1) {
                stream->in_departures = false;
            }
            break;

        default:
            break;
        }
    }
}

/* Feeds a successful response to the stream, other responses are only checked by status */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && esp_http_client_get_status_code(evt->client) == 200) {
        stream_feed(evt->user_data, evt->data, evt->data_len);
    }

    return ESP_OK;
}

/* Starts with a number: compared numerically, so 2 < 10 < 515 and 3 < 3B */
static int compare_lines(const char *a, const char *b)
{
    char *a_rest, *b_rest;
    long a_num = strtol(a, &a_rest, 10);
    long b_num = strtol(b, &b_rest, 10);

    bool a_has_num = a_rest != a;
    bool b_has_num = b_rest != b;

    if (a_has_num != b_has_num) {
        return a_has_num ? -1 : 1;
    }
    if (a_has_num && a_num != b_num) {
        return a_num < b_num ? -1 : 1;
    }
    return strcmp(a_rest, b_rest);
}

static int compare_options(const void *a, const void *b)
{
    const gtg_departure_option_t *x = a, *y = b;

    int cmp = compare_lines(x->line, y->line);
    if (cmp == 0) {
        cmp = strcmp(x->mode, y->mode);
    }
    if (cmp == 0) {
        cmp = strcmp(x->destination, y->destination);
    }
    return cmp;
}

esp_err_t gtg_departures_list_options(const char *stop_id, gtg_departure_option_t *options, size_t *count)
{
    if (stop_id == NULL || options == NULL || count == NULL || !is_valid_stop_id(stop_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;

    char key[GTG_API_KEY_MAX_LEN + 1];
    if (gtg_api_keys_get(GTG_API_KEY_TRAFIKLAB, key, sizeof(key)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[DEPARTURES_URL_MAX_LEN];
    snprintf(url, sizeof(url), DEPARTURES_URL, stop_id, key);
    memset(key, 0, sizeof(key));

    departure_stream_t stream = {
        .departure = malloc(DEPARTURE_JSON_MAX_LEN),
        .options   = options,
    };
    if (stream.departure == NULL) {
        memset(url, 0, sizeof(url));
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url               = url,
        .timeout_ms        = API_REQUEST_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event_handler,
        .user_data         = &stream,
    };

    esp_err_t err = ESP_ERR_NO_MEM;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != NULL) {
        err = esp_http_client_set_header(client, "Accept", "application/json");
        if (err == ESP_OK) {
            err = esp_http_client_perform(client);
        }

        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Trafiklab request failed: %s", esp_err_to_name(err));
        } else if (status == 404) {
            err = ESP_ERR_NOT_FOUND;
        } else if (status == 401 || status == 403) {
            err = ESP_ERR_NOT_ALLOWED;
        } else if (status != 200 || stream.depth != 0 || stream.in_string) {
            /* a cut response leaves the nesting open */
            ESP_LOGE(TAG, "unexpected response, status %d, depth %d", status, stream.depth);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }

    memset(url, 0, sizeof(url));
    free(stream.departure);

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "%u departures from %s, %u unique, %u skipped, %u dropped",
             (unsigned)stream.departures, stop_id, (unsigned)stream.count,
             (unsigned)stream.skipped, (unsigned)stream.dropped);

    qsort(options, stream.count, sizeof(options[0]), compare_options);
    *count = stream.count;

    return ESP_OK;
}
