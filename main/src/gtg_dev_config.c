#include "gtg_dev_config.h"

#include <stdlib.h>
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

    memset(&s_config, 0, sizeof(s_config));

    /* the cap is small, so all targets get room up front and adding one can't fail on memory */
    s_config.realtime_targets.list = calloc(GTG_DEV_CONFIG_MAX_TARGETS, sizeof(trafiklab_target_t));
    if (s_config.realtime_targets.list == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_config.realtime_targets.size = GTG_DEV_CONFIG_MAX_TARGETS;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        free(s_config.realtime_targets.list);
        s_config.realtime_targets.list = NULL;
        s_config.realtime_targets.size = 0;
        return ESP_ERR_NO_MEM;
    }

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

/* True when the field holds a NUL terminated string, non-empty when required */
static bool is_terminated(const char *field, size_t field_size, bool required)
{
    size_t len = strnlen(field, field_size);
    return len < field_size && (!required || len > 0);
}

static bool is_valid_target(const trafiklab_target_t *target)
{
    const gtg_stop_t *stop = &target->stop;
    const gtg_departure_option_t *departure = &target->departure;

    return is_terminated(stop->id, sizeof(stop->id), true)
        && is_terminated(stop->name, sizeof(stop->name), false)
        && is_terminated(stop->town, sizeof(stop->town), false)
        && is_terminated(departure->line, sizeof(departure->line), false)
        && is_terminated(departure->mode, sizeof(departure->mode), true)
        && is_terminated(departure->destination, sizeof(departure->destination), true);
}

/* Index of the target, or -1 when it is not tracked. Called with the lock held */
static int target_index(const char *stop_id, const gtg_departure_option_t *departure)
{
    const trafiklab_target_list_t *targets = &s_config.realtime_targets;

    for (size_t i = 0; i < targets->len; i++) {
        const trafiklab_target_t *target = &targets->list[i];

        if (strcmp(target->stop.id, stop_id) == 0
            && strcmp(target->departure.line, departure->line) == 0
            && strcmp(target->departure.mode, departure->mode) == 0
            && strcmp(target->departure.destination, departure->destination) == 0) {
            return (int)i;
        }
    }

    return -1;
}

/* Called with the lock held */
static bool find_target(const char *stop_id, const gtg_departure_option_t *departure)
{
    return target_index(stop_id, departure) >= 0;
}

esp_err_t gtg_dev_config_add_target(const trafiklab_target_t *target, size_t *count)
{
    if (target == NULL || !is_valid_target(target)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    trafiklab_target_list_t *targets = &s_config.realtime_targets;

    esp_err_t err = ESP_OK;
    if (find_target(target->stop.id, &target->departure)) {
        err = ESP_ERR_NOT_ALLOWED;
    } else if (targets->len >= targets->size) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        targets->list[targets->len++] = *target;
        mark_changed();
    }

    size_t len = targets->len;

    xSemaphoreGive(s_mutex);

    if (count != NULL) {
        *count = len;
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "tracking %s %s %s from %s (%s), %u of %d",
                 target->departure.mode, target->departure.line, target->departure.destination,
                 target->stop.name, target->stop.id, (unsigned)len, GTG_DEV_CONFIG_MAX_TARGETS);
    }

    return err;
}

esp_err_t gtg_dev_config_remove_target(const char *stop_id, const gtg_departure_option_t *departure)
{
    if (stop_id == NULL || departure == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    trafiklab_target_list_t *targets = &s_config.realtime_targets;
    int index = target_index(stop_id, departure);

    trafiklab_target_t removed = { 0 };
    if (index >= 0) {
        removed = targets->list[index];

        /* the ones after it move up, so the order they were added in stays */
        memmove(&targets->list[index], &targets->list[index + 1],
                (targets->len - index - 1) * sizeof(targets->list[0]));
        targets->len--;
        memset(&targets->list[targets->len], 0, sizeof(targets->list[0]));
        mark_changed();
    }

    size_t len = targets->len;

    xSemaphoreGive(s_mutex);

    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "stopped tracking %s %s %s from %s (%s), %u of %d",
             removed.departure.mode, removed.departure.line, removed.departure.destination,
             removed.stop.name, removed.stop.id, (unsigned)len, GTG_DEV_CONFIG_MAX_TARGETS);

    return ESP_OK;
}

esp_err_t gtg_dev_config_get_targets(trafiklab_target_t *out, size_t max, size_t *count)
{
    if (out == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;

    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    const trafiklab_target_list_t *targets = &s_config.realtime_targets;
    size_t copied = targets->len < max ? targets->len : max;
    memcpy(out, targets->list, copied * sizeof(*out));
    bool fits = targets->len <= max;

    xSemaphoreGive(s_mutex);

    *count = copied;
    return fits ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

bool gtg_dev_config_has_target(const char *stop_id, const gtg_departure_option_t *departure)
{
    if (stop_id == NULL || departure == NULL || s_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool found = find_target(stop_id, departure);
    xSemaphoreGive(s_mutex);

    return found;
}
