#include "gtg_dev_config.h"

#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "gtg_ntp.h"

static const char *TAG = "dev config";

static gtg_dev_config_t s_config;
static SemaphoreHandle_t s_mutex = NULL;

/* The field that holds the key of each service */
static char *api_key_field(gtg_api_key_t which)
{
    switch (which) {
    case GTG_API_KEY_TRAFIKLAB: return s_config.trafiklab_api_key;
    case GTG_API_KEY_GOOGLE:    return s_config.google_maps_api_key;
    default:                    return NULL;
    }
}

/* Called with the lock held */
static void mark_changed(void)
{
    s_config.last_changed = gtg_ntp_is_time_set() ? time(NULL) : 0;
}

/* Copies a string into a field, true when the field changed */
static bool update_field(char *field, size_t field_size, const char *value)
{
    if (strncmp(field, value, field_size) == 0) {
        return false;
    }

    strlcpy(field, value, field_size);
    return true;
}

esp_err_t gtg_dev_config_init(void)
{
    if (s_mutex != NULL) {
        ESP_LOGE(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_config, 0, sizeof(s_config));

    return ESP_OK;
}

esp_err_t gtg_dev_config_set_wifi(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strnlen(ssid, GTG_DEV_CONFIG_SSID_MAX_LEN + 1);
    if (ssid_len == 0 || ssid_len > GTG_DEV_CONFIG_SSID_MAX_LEN
        || strnlen(pass, GTG_DEV_CONFIG_PASS_MAX_LEN + 1) > GTG_DEV_CONFIG_PASS_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* both are compared, so a changed password alone also counts */
    bool changed = update_field(s_config.wifi_ssid, sizeof(s_config.wifi_ssid), ssid);
    changed = update_field(s_config.wifi_pass, sizeof(s_config.wifi_pass), pass) || changed;
    if (changed) {
        mark_changed();
    }

    xSemaphoreGive(s_mutex);

    if (changed) {
        ESP_LOGI(TAG, "WiFi credentials stored for SSID:%s", ssid);
    }

    return ESP_OK;
}

esp_err_t gtg_dev_config_set_api_key(gtg_api_key_t which, const char *key)
{
    char *field = api_key_field(which);
    if (field == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strnlen(key, GTG_API_KEY_MAX_LEN + 1);
    if (len == 0 || len > GTG_API_KEY_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool changed = update_field(field, GTG_API_KEY_MAX_LEN + 1, key);
    if (changed) {
        mark_changed();
    }

    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

esp_err_t gtg_dev_config_get_api_key(gtg_api_key_t which, char *out, size_t out_size)
{
    const char *field = api_key_field(which);
    if (field == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    if (field[0] == '\0') {
        err = ESP_ERR_NOT_FOUND;
    } else if (strlcpy(out, field, out_size) >= out_size) {
        err = ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreGive(s_mutex);

    /* a cut key must not end up in a request */
    if (err != ESP_OK && out_size > 0) {
        memset(out, 0, out_size);
    }

    return err;
}
