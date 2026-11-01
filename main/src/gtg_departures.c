#include "gtg_departures.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "gtg_api_keys.h"
#include "gtg_dev_config.h"
#include "gtg_str.h"
#include "gtg_events.h"
#include "gtg_stop_search.h"
#include "gtg_ntp.h"

#define API_REQUEST_TIMEOUT_MS  10000

/* {time} is local time as YYYY-MM-DDTHH:mm, the hour after it is returned */
#define DEPARTURES_URL          "https://realtime-api.trafiklab.se/v1/departures/%s/%s?key=%s"
#define SAMPLE_TIME_LEN         sizeof("YYYY-MM-DDTHH:mm")
#define DEPARTURES_URL_MAX_LEN  (sizeof(DEPARTURES_URL) + GTG_STOP_ID_MAX_LEN + SAMPLE_TIME_LEN + GTG_API_KEY_MAX_LEN)

/* without a time the departures from now on are returned, shorter than DEPARTURES_URL */
#define UPCOMING_URL            "https://realtime-api.trafiklab.se/v1/departures/%s?key=%s"

/* 8 samples from 00:00 end at 20:53, all within the day */
#define SAMPLE_DAY              3       /* tm_wday of Wednesday */
#define SAMPLE_STEP_MIN         (2 * 60 + 59)
_Static_assert((GTG_DEPARTURES_SAMPLES - 1) * SAMPLE_STEP_MIN < 24 * 60, "samples must stay within the day");

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
typedef struct departure_stream departure_stream_t;

/* Called for each departure with a route, *option is its line, mode and destination */
typedef void (*departure_handler_t)(departure_stream_t *stream, const cJSON *json,
                                    const gtg_departure_option_t *option);

struct departure_stream {
    departure_handler_t on_departure;

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

    size_t departures;          /* departures seen */
    size_t skipped;             /* departures too long or not understood */

    /* gtg_departures_list_options() */
    gtg_departure_option_t *options;
    size_t                  count;
    size_t                  dropped;        /* unique options that didn't fit */

    /* gtg_departures_fetch_upcoming() */
    const gtg_departure_option_t *targets;
    size_t                        target_count;
    gtg_departures_upcoming_t    *upcoming;
    time_t                        now;
    size_t                        matched;  /* departures that matched a target in the window */
};

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

/* The departure handler of gtg_departures_list_options(), keeps each unique option */
static void add_option(departure_stream_t *stream, const cJSON *json, const gtg_departure_option_t *option)
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

/* Reads a local time like "2026-10-31T12:37:22", as Trafiklab sends them */
static bool parse_local_time(const char *text, time_t *out)
{
    struct tm tm = { 0 };
    if (text == NULL || sscanf(text, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        return false;
    }

    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    /* daylight saving time is worked out by mktime from the TZ set in gtg_ntp.c */
    tm.tm_isdst = -1;

    *out = mktime(&tm);
    return *out != (time_t)-1;
}

/* Puts seconds into the upcoming departures, which are kept as the soonest ones, in order */
static void insert_upcoming(gtg_departures_upcoming_t *upcoming, uint16_t seconds)
{
    size_t pos = upcoming->count;
    while (pos > 0 && upcoming->seconds[pos - 1] > seconds) {
        pos--;
    }
    if (pos >= GTG_DEPARTURES_UPCOMING_MAX) {
        return;
    }

    size_t last = upcoming->count < GTG_DEPARTURES_UPCOMING_MAX ? upcoming->count : GTG_DEPARTURES_UPCOMING_MAX - 1;
    for (size_t i = last; i > pos; i--) {
        upcoming->seconds[i] = upcoming->seconds[i - 1];
    }
    upcoming->seconds[pos] = seconds;

    if (upcoming->count < GTG_DEPARTURES_UPCOMING_MAX) {
        upcoming->count++;
    }
}

/* The departure handler of gtg_departures_fetch_upcoming(). A departure within the window is
 * added to every target it matches, canceled ones are skipped */
static void add_upcoming(departure_stream_t *stream, const cJSON *json, const gtg_departure_option_t *option)
{
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "canceled"))) {
        return;
    }

    /* the realtime estimate when there is one, the timetable otherwise */
    const char *when = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "realtime"));
    if (when == NULL) {
        when = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "scheduled"));
    }

    time_t departs;
    if (!parse_local_time(when, &departs)) {
        stream->skipped++;
        return;
    }

    /* a delayed departure can be listed although it has left */
    double seconds = difftime(departs, stream->now);
    if (seconds < 0 || seconds > GTG_DEPARTURES_UPCOMING_WINDOW_S) {
        return;
    }

    for (size_t i = 0; i < stream->target_count; i++) {
        if (same_option(&stream->targets[i], option)) {
            insert_upcoming(&stream->upcoming[i], (uint16_t)seconds);
            stream->matched++;
        }
    }
}

/* Reads the route of one departure into an option and hands it to the stream's handler */
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
        /* copied like the saved targets are, so they compare equal */
        gtg_departure_option_t option;
        gtg_copy_utf8(option.line, sizeof(option.line), line != NULL ? line : "");
        gtg_copy_utf8(option.mode, sizeof(option.mode), mode);
        gtg_copy_utf8(option.destination, sizeof(option.destination), destination);
        stream->on_departure(stream, json, &option);
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

    int cmp = strcmp(x->mode, y->mode);
    if (cmp == 0) {
        cmp = compare_lines(x->line, y->line);
    }
    if (cmp == 0) {
        cmp = strcmp(x->destination, y->destination);
    }
    return cmp;
}

/* Finds the date of the next Wednesday in local time, a week ahead when today is Wednesday */
static esp_err_t next_sample_day(struct tm *day)
{
    struct tm now;
    if (gtg_ntp_get_local_time(&now) != ESP_OK) {
        return ESP_ERR_NOT_FINISHED;
    }

    int days_ahead = (SAMPLE_DAY - now.tm_wday + 7) % 7;
    if (days_ahead == 0) {
        days_ahead = 7;
    }

    /* mktime moves the date into the next month or year when needed. Noon keeps a
     * daylight saving change during the night from moving it to another day */
    *day = now;
    day->tm_mday += days_ahead;
    day->tm_hour = 12;
    day->tm_min = 0;
    day->tm_sec = 0;
    day->tm_isdst = -1;

    time_t t = mktime(day);
    if (t == (time_t)-1) {
        return ESP_FAIL;
    }
    localtime_r(&t, day);

    return ESP_OK;
}

/* Resets the stream for the next response, the options found so far are kept */
static void stream_reset(departure_stream_t *stream)
{
    stream->depth = 0;
    stream->in_string = false;
    stream->escape = false;
    stream->in_departures = false;
    stream->key_len = 0;
    stream->collecting = false;
    stream->too_long = false;
}

/* Requests and parses one sample. The client keeps its connection open for the next one */
static esp_err_t fetch_sample(esp_http_client_handle_t client, const char *url, departure_stream_t *stream)
{
    stream_reset(stream);

    esp_err_t err = esp_http_client_set_url(client, url);
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    if (err != ESP_OK) {
        return err;
    }

    int status = esp_http_client_get_status_code(client);
    if (status == 404) {
        return ESP_ERR_NOT_FOUND;
    }
    if (status == 401 || status == 403) {
        /* the search stops on this, so it is posted once per search */
        gtg_events_post_key_rejected(GTG_API_KEY_TRAFIKLAB);
        return ESP_ERR_NOT_ALLOWED;
    }
    if (status != 200 || stream->depth != 0 || stream->in_string) {
        /* a cut response leaves the nesting open, the departures read before it are kept */
        ESP_LOGW(TAG, "unexpected response, status %d, depth %d", status, stream->depth);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t gtg_departures_list_options(const char *stop_id, gtg_departure_option_t *options,
                                      size_t *count, size_t *failed)
{
    if (stop_id == NULL || options == NULL || count == NULL || failed == NULL || !is_valid_stop_id(stop_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;
    *failed = 0;

    struct tm day;
    ESP_RETURN_ON_ERROR(next_sample_day(&day), TAG, "the clock is not set");

    char key[GTG_API_KEY_MAX_LEN + 1];
    if (gtg_dev_config_get_api_key(GTG_API_KEY_TRAFIKLAB, key, sizeof(key)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    departure_stream_t stream = {
        .on_departure = add_option,
        .departure    = malloc(DEPARTURE_JSON_MAX_LEN),
        .options      = options,
    };

    /* the URL is set per sample, the first one only creates the client */
    char url[DEPARTURES_URL_MAX_LEN] = "https://realtime-api.trafiklab.se/";
    esp_http_client_config_t config = {
        .url               = url,
        .timeout_ms        = API_REQUEST_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event_handler,
        .user_data         = &stream,
    };

    esp_http_client_handle_t client = stream.departure != NULL ? esp_http_client_init(&config) : NULL;
    esp_err_t err = client != NULL ? esp_http_client_set_header(client, "Accept", "application/json")
                                   : ESP_ERR_NO_MEM;

    /* the last failure is returned when every sample fails */
    esp_err_t last_err = ESP_OK;
    for (int i = 0; i < GTG_DEPARTURES_SAMPLES && err == ESP_OK; i++) {
        struct tm sample = day;
        sample.tm_hour = i * SAMPLE_STEP_MIN / 60;
        sample.tm_min = i * SAMPLE_STEP_MIN % 60;

        char sample_time[SAMPLE_TIME_LEN];
        strftime(sample_time, sizeof(sample_time), "%Y-%m-%dT%H:%M", &sample);
        snprintf(url, sizeof(url), DEPARTURES_URL, stop_id, sample_time, key);

        size_t before = stream.count;
        esp_err_t sample_err = fetch_sample(client, url, &stream);

        if (sample_err == ESP_OK) {
            ESP_LOGI(TAG, "sample %d/%d at %s: %u new, %u unique", i + 1, GTG_DEPARTURES_SAMPLES, sample_time,
                     (unsigned)(stream.count - before), (unsigned)stream.count);
        } else {
            ESP_LOGW(TAG, "sample %d/%d at %s failed: %s", i + 1, GTG_DEPARTURES_SAMPLES, sample_time,
                     esp_err_to_name(sample_err));
            (*failed)++;
            last_err = sample_err;

            /* an unknown stop or a rejected key fails every sample the same way */
            if (sample_err == ESP_ERR_NOT_FOUND || sample_err == ESP_ERR_NOT_ALLOWED) {
                err = sample_err;
            }
        }
    }

    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    memset(url, 0, sizeof(url));
    memset(key, 0, sizeof(key));
    free(stream.departure);

    if (err == ESP_OK && *failed == GTG_DEPARTURES_SAMPLES) {
        err = last_err;
    }
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "%u departures from %s, %u unique, %u skipped, %u dropped, %u of %d samples failed",
             (unsigned)stream.departures, stop_id, (unsigned)stream.count, (unsigned)stream.skipped,
             (unsigned)stream.dropped, (unsigned)*failed, GTG_DEPARTURES_SAMPLES);

    qsort(options, stream.count, sizeof(options[0]), compare_options);
    *count = stream.count;

    return ESP_OK;
}

esp_err_t gtg_departures_fetch_upcoming(const char *stop_id, const gtg_departure_option_t *targets,
                                        size_t count, gtg_departures_upcoming_t *upcoming,
                                        time_t *fetched_at)
{
    if (stop_id == NULL || targets == NULL || upcoming == NULL || fetched_at == NULL
        || count == 0 || !is_valid_stop_id(stop_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(upcoming, 0, count * sizeof(upcoming[0]));

    if (!gtg_ntp_is_time_set()) {
        return ESP_ERR_NOT_FINISHED;
    }

    char key[GTG_API_KEY_MAX_LEN + 1];
    if (gtg_dev_config_get_api_key(GTG_API_KEY_TRAFIKLAB, key, sizeof(key)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    departure_stream_t stream = {
        .on_departure = add_upcoming,
        .departure    = malloc(DEPARTURE_JSON_MAX_LEN),
        .targets      = targets,
        .target_count = count,
        .upcoming     = upcoming,
        .now          = time(NULL),
    };

    char url[DEPARTURES_URL_MAX_LEN];
    snprintf(url, sizeof(url), UPCOMING_URL, stop_id, key);

    esp_http_client_config_t config = {
        .url               = url,
        .timeout_ms        = API_REQUEST_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event_handler,
        .user_data         = &stream,
    };

    esp_http_client_handle_t client = stream.departure != NULL ? esp_http_client_init(&config) : NULL;
    esp_err_t err = client != NULL ? esp_http_client_set_header(client, "Accept", "application/json")
                                   : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = fetch_sample(client, url, &stream);
    }

    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    memset(url, 0, sizeof(url));
    memset(key, 0, sizeof(key));
    free(stream.departure);

    if (err != ESP_OK) {
        /* a cut response may have filled some, they aren't complete */
        memset(upcoming, 0, count * sizeof(upcoming[0]));
        ESP_LOGW(TAG, "upcoming departures from %s failed: %s", stop_id, esp_err_to_name(err));
        return err;
    }

    *fetched_at = stream.now;

    ESP_LOGI(TAG, "%u departures from %s, %u matched %u targets within %d s, %u skipped",
             (unsigned)stream.departures, stop_id, (unsigned)stream.matched, (unsigned)count,
             GTG_DEPARTURES_UPCOMING_WINDOW_S, (unsigned)stream.skipped);

    return ESP_OK;
}
