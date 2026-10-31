#include "gtg_api_keys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "gtg_dev_config.h"

#define API_REQUEST_TIMEOUT_MS  10000
#define API_URL_MAX_LEN         256

#define TRAFIKLAB_TEST_URL      "https://realtime-api.trafiklab.se/v1/stops/name/T-centralen?key=%s"

/* result_type=country keeps the response to a few hundred bytes, so it can be parsed in one buffer */
#define GOOGLE_TEST_URL         "https://maps.googleapis.com/maps/api/geocode/json?latlng=59.33166,18.061694&result_type=country&key=%s"
#define GOOGLE_BODY_MAX_LEN     2048

/* one bit per gtg_api_key_t, set while a verified key is stored */
#define ALL_KEYS_SET_BITS       ((1 << GTG_API_KEY_COUNT) - 1)

static const char *TAG = "api keys";

static EventGroupHandle_t s_event_group = NULL;

/* Sends a GET request. When body is given the response body is read into it and NUL terminated */
static esp_err_t http_get(const char *url, int *status, char *body, size_t body_size)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = API_REQUEST_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_set_header(client, "Accept", "application/json");
    if (err == ESP_OK) {
        err = esp_http_client_open(client, 0);
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        *status = esp_http_client_get_status_code(client);

        if (body != NULL) {
            int len = esp_http_client_read_response(client, body, body_size - 1);
            if (len < 0) {
                err = ESP_FAIL;
            } else {
                body[len] = '\0';
            }
        }
    }

    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t check_trafiklab_key(const char *key)
{
    char url[API_URL_MAX_LEN];
    snprintf(url, sizeof(url), TRAFIKLAB_TEST_URL, key);

    int status;
    ESP_RETURN_ON_ERROR(http_get(url, &status, NULL, 0), TAG, "Trafiklab request failed");

    ESP_LOGI(TAG, "Trafiklab responded with %d", status);

    if (status >= 200 && status < 300) {
        return ESP_OK;
    }
    if (status >= 400 && status < 500) {
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

/* The Geocoding API responds with 200 also for rejected keys, the result is in "status" */
static esp_err_t parse_google_response(int status, const char *body)
{
    if (status >= 400 && status < 500) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (status != 200) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *json = cJSON_Parse(body);
    const char *result = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "status"));
    const char *message = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "error_message"));

    esp_err_t err;
    if (result == NULL) {
        err = ESP_ERR_INVALID_RESPONSE;
    } else if (strcmp(result, "OK") == 0 || strcmp(result, "ZERO_RESULTS") == 0) {
        err = ESP_OK;
    } else if (strcmp(result, "REQUEST_DENIED") == 0) {
        err = ESP_ERR_NOT_ALLOWED;
    } else {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "Google responded with %s%s%s", result != NULL ? result : "an invalid body",
             message != NULL ? ": " : "", message != NULL ? message : "");

    cJSON_Delete(json);
    return err;
}

static esp_err_t check_google_key(const char *key)
{
    char url[API_URL_MAX_LEN];
    snprintf(url, sizeof(url), GOOGLE_TEST_URL, key);

    char *body = malloc(GOOGLE_BODY_MAX_LEN);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int status;
    esp_err_t err = http_get(url, &status, body, GOOGLE_BODY_MAX_LEN);
    if (err == ESP_OK) {
        err = parse_google_response(status, body);
    } else {
        ESP_LOGE(TAG, "Google request failed: %s", esp_err_to_name(err));
    }

    free(body);
    return err;
}

typedef struct {
    const char *name;
    esp_err_t (*check)(const char *key);
} api_service_t;

static const api_service_t SERVICES[GTG_API_KEY_COUNT] = {
    [GTG_API_KEY_TRAFIKLAB] = { "Trafiklab", check_trafiklab_key },
    [GTG_API_KEY_GOOGLE]    = { "Google",    check_google_key },
};

static bool is_valid_service(gtg_api_key_t which)
{
    return (int)which >= 0 && which < GTG_API_KEY_COUNT;
}

/* the key is put in a URL as is, so only characters that need no encoding are accepted */
static bool is_valid_key(const char *key)
{
    size_t len = strnlen(key, GTG_API_KEY_MAX_LEN + 1);
    if (len == 0 || len > GTG_API_KEY_MAX_LEN) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)key[i];
        if (!isalnum(c) && c != '-' && c != '_') {
            return false;
        }
    }

    return true;
}

esp_err_t gtg_api_keys_init(void)
{
    if (s_event_group != NULL) {
        ESP_LOGE(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t gtg_api_keys_set(gtg_api_key_t which, const char *key)
{
    if (!is_valid_service(which) || key == NULL || !is_valid_key(key)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const api_service_t *service = &SERVICES[which];

    esp_err_t err = service->check(key);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s key not accepted: %s", service->name, esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(gtg_dev_config_set_api_key(which, key), TAG, "failed to store the %s key", service->name);

    xEventGroupSetBits(s_event_group, BIT(which));

    ESP_LOGI(TAG, "%s key verified", service->name);

    return ESP_OK;
}

bool gtg_api_keys_is_set(gtg_api_key_t which)
{
    if (!is_valid_service(which) || s_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_event_group) & BIT(which)) != 0;
}

esp_err_t gtg_api_keys_wait_configured(TickType_t ticks)
{
    if (s_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
            ALL_KEYS_SET_BITS,
            pdFALSE,
            pdTRUE,
            ticks);

    return (bits & ALL_KEYS_SET_BITS) == ALL_KEYS_SET_BITS ? ESP_OK : ESP_ERR_TIMEOUT;
}
