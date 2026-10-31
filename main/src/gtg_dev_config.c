#include "gtg_dev_config.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

#include "gtg_ntp.h"

/*
 * NVS layout, one key per field in the "gtg_cfg" namespace. Key names are at most 15 characters.
 * The targets are stored as one blob of trafiklab_target_t, so their size is stored next to them:
 * a changed struct layout is then rejected on load instead of read wrongly. Bump NVS_VERSION when
 * the meaning of a key changes.
 */
#define NVS_NAMESPACE           "gtg_cfg"
#define NVS_VERSION             1
#define NVS_KEY_VERSION         "version"
#define NVS_KEY_WIFI_SSID       "wifi_ssid"
#define NVS_KEY_WIFI_PASS       "wifi_pass"
#define NVS_KEY_TRAFIKLAB       "trafiklab_key"
#define NVS_KEY_GOOGLE          "google_key"
#define NVS_KEY_LAST_CHANGED    "last_changed"
#define NVS_KEY_TARGET_SIZE     "target_size"
#define NVS_KEY_TARGETS         "targets"

static const char *TAG = "dev config";

static gtg_dev_config_t s_config;
static SemaphoreHandle_t s_mutex = NULL;

static esp_err_t save_locked(void);

/* The field that holds the key of each service */
static char *api_key_field(gtg_api_key_t which)
{
    switch (which) {
    case GTG_API_KEY_TRAFIKLAB: return s_config.trafiklab_api_key;
    case GTG_API_KEY_GOOGLE:    return s_config.google_maps_api_key;
    default:                    return NULL;
    }
}

/* Stamps the change and saves the configuration. A failed save is only logged, the change
 * stays in RAM and the next change saves again. Called with the lock held */
static void mark_changed(void)
{
    s_config.last_changed = gtg_ntp_is_time_set() ? time(NULL) : 0;

    esp_err_t err = save_locked();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to save the configuration: %s", esp_err_to_name(err));
    }
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

/* Writes every field and commits. Called with the lock held */
static esp_err_t save_locked(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    const trafiklab_target_list_t *targets = &s_config.realtime_targets;

    err = nvs_set_u16(nvs, NVS_KEY_VERSION, NVS_VERSION);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_WIFI_SSID, s_config.wifi_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_WIFI_PASS, s_config.wifi_pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_TRAFIKLAB, s_config.trafiklab_api_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_GOOGLE, s_config.google_maps_api_key);
    }
    if (err == ESP_OK) {
        err = nvs_set_i64(nvs, NVS_KEY_LAST_CHANGED, (int64_t)s_config.last_changed);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, NVS_KEY_TARGET_SIZE, sizeof(trafiklab_target_t));
    }
    if (err == ESP_OK) {
        if (targets->len > 0) {
            err = nvs_set_blob(nvs, NVS_KEY_TARGETS, targets->list, targets->len * sizeof(targets->list[0]));
        } else {
            /* no targets are stored as no key */
            err = nvs_erase_key(nvs, NVS_KEY_TARGETS);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "configuration saved, %u targets", (unsigned)targets->len);
    }

    return err;
}

esp_err_t gtg_dev_config_save(void)
{
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);

    return err;
}

/* Reads a string key into a field. A missing key reads as an empty string */
static esp_err_t load_str(nvs_handle_t nvs, const char *key, char *field, size_t field_size)
{
    size_t len = field_size;
    esp_err_t err = nvs_get_str(nvs, key, field, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        field[0] = '\0';
        return ESP_OK;
    }
    return err;
}

/* Reads the stored targets into list, room for GTG_DEV_CONFIG_MAX_TARGETS */
static esp_err_t load_targets(nvs_handle_t nvs, trafiklab_target_t *list, size_t *count)
{
    *count = 0;

    uint16_t target_size = 0;
    esp_err_t err = nvs_get_u16(nvs, NVS_KEY_TARGET_SIZE, &target_size);
    if (err != ESP_OK) {
        return err;
    }
    if (target_size != sizeof(trafiklab_target_t)) {
        ESP_LOGW(TAG, "stored targets have another layout (%u bytes, now %u)",
                 target_size, (unsigned)sizeof(trafiklab_target_t));
        return ESP_ERR_INVALID_VERSION;
    }

    size_t len = 0;
    err = nvs_get_blob(nvs, NVS_KEY_TARGETS, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (len % sizeof(trafiklab_target_t) != 0 || len > GTG_DEV_CONFIG_MAX_TARGETS * sizeof(trafiklab_target_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = nvs_get_blob(nvs, NVS_KEY_TARGETS, list, &len);
    if (err != ESP_OK) {
        return err;
    }

    size_t loaded = len / sizeof(trafiklab_target_t);
    for (size_t i = 0; i < loaded; i++) {
        if (!is_valid_target(&list[i])) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    *count = loaded;
    return ESP_OK;
}

/* Reads and checks the stored configuration into *out without touching the runtime one.
 * out->realtime_targets.list must have room for GTG_DEV_CONFIG_MAX_TARGETS */
static esp_err_t read_stored(gtg_dev_config_t *out)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint16_t version = 0;
    int64_t last_changed = 0;

    err = nvs_get_u16(nvs, NVS_KEY_VERSION, &version);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    } else if (err == ESP_OK && version != NVS_VERSION) {
        ESP_LOGW(TAG, "stored configuration has version %u, expected %u", version, NVS_VERSION);
        err = ESP_ERR_INVALID_VERSION;
    }
    if (err == ESP_OK) {
        err = load_str(nvs, NVS_KEY_WIFI_SSID, out->wifi_ssid, sizeof(out->wifi_ssid));
    }
    if (err == ESP_OK) {
        err = load_str(nvs, NVS_KEY_WIFI_PASS, out->wifi_pass, sizeof(out->wifi_pass));
    }
    if (err == ESP_OK) {
        err = load_str(nvs, NVS_KEY_TRAFIKLAB, out->trafiklab_api_key, sizeof(out->trafiklab_api_key));
    }
    if (err == ESP_OK) {
        err = load_str(nvs, NVS_KEY_GOOGLE, out->google_maps_api_key, sizeof(out->google_maps_api_key));
    }
    if (err == ESP_OK) {
        err = nvs_get_i64(nvs, NVS_KEY_LAST_CHANGED, &last_changed);
    }
    if (err == ESP_OK) {
        err = load_targets(nvs, out->realtime_targets.list, &out->realtime_targets.len);
    }

    nvs_close(nvs);

    out->last_changed = (time_t)last_changed;
    return err;
}

/* A copy of the configuration with room for the targets, to read the stored one into */
static esp_err_t stored_copy_create(gtg_dev_config_t *copy)
{
    memset(copy, 0, sizeof(*copy));
    copy->realtime_targets.list = calloc(GTG_DEV_CONFIG_MAX_TARGETS, sizeof(trafiklab_target_t));
    copy->realtime_targets.size = GTG_DEV_CONFIG_MAX_TARGETS;
    return copy->realtime_targets.list != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

/* The copy holds the password and keys, so it is wiped before it is freed */
static void stored_copy_free(gtg_dev_config_t *copy)
{
    free(copy->realtime_targets.list);
    memset(copy, 0, sizeof(*copy));
}

esp_err_t gtg_dev_config_load(void)
{
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* read into a copy, so a stored configuration that turns out invalid leaves the current one */
    gtg_dev_config_t loaded;
    esp_err_t err = stored_copy_create(&loaded);
    if (err == ESP_OK) {
        err = read_stored(&loaded);
    }

    if (err == ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);

        memcpy(s_config.wifi_ssid, loaded.wifi_ssid, sizeof(s_config.wifi_ssid));
        memcpy(s_config.wifi_pass, loaded.wifi_pass, sizeof(s_config.wifi_pass));
        memcpy(s_config.trafiklab_api_key, loaded.trafiklab_api_key, sizeof(s_config.trafiklab_api_key));
        memcpy(s_config.google_maps_api_key, loaded.google_maps_api_key, sizeof(s_config.google_maps_api_key));
        s_config.last_changed = loaded.last_changed;

        trafiklab_target_list_t *targets = &s_config.realtime_targets;
        memset(targets->list, 0, targets->size * sizeof(targets->list[0]));
        memcpy(targets->list, loaded.realtime_targets.list, loaded.realtime_targets.len * sizeof(targets->list[0]));
        targets->len = loaded.realtime_targets.len;

        xSemaphoreGive(s_mutex);

        ESP_LOGI(TAG, "configuration loaded, SSID:%s, %u targets",
                 loaded.wifi_ssid, (unsigned)loaded.realtime_targets.len);
    } else if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "stored configuration not loaded: %s", esp_err_to_name(err));
    }

    stored_copy_free(&loaded);
    return err;
}

/* "set (N chars)" or "not set", the value itself is never logged */
static const char *secret_state(const char *value, char *buf, size_t buf_size)
{
    if (value[0] == '\0') {
        return "not set";
    }
    snprintf(buf, buf_size, "set (%u chars)", (unsigned)strlen(value));
    return buf;
}

void gtg_dev_config_log_stored(void)
{
    gtg_dev_config_t stored;
    esp_err_t err = stored_copy_create(&stored);
    if (err == ESP_OK) {
        err = read_stored(&stored);
    }

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "stored configuration: none");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "stored configuration: unreadable, %s", esp_err_to_name(err));
    } else {
        char buf[24];

        /* the timezone is set later by NTP, so the time is shown in UTC */
        char changed[32] = "unknown";
        if (stored.last_changed != 0) {
            struct tm tm;
            gmtime_r(&stored.last_changed, &tm);
            strftime(changed, sizeof(changed), "%Y-%m-%d %H:%M:%S UTC", &tm);
        }

        ESP_LOGI(TAG, "stored configuration:");
        ESP_LOGI(TAG, "  last changed:  %s", changed);
        ESP_LOGI(TAG, "  WiFi SSID:     %s", stored.wifi_ssid[0] != '\0' ? stored.wifi_ssid : "not set");
        ESP_LOGI(TAG, "  WiFi password: %s", secret_state(stored.wifi_pass, buf, sizeof(buf)));
        ESP_LOGI(TAG, "  Trafiklab key: %s", secret_state(stored.trafiklab_api_key, buf, sizeof(buf)));
        ESP_LOGI(TAG, "  Google key:    %s", secret_state(stored.google_maps_api_key, buf, sizeof(buf)));
        ESP_LOGI(TAG, "  targets:       %u of %d",
                 (unsigned)stored.realtime_targets.len, GTG_DEV_CONFIG_MAX_TARGETS);

        for (size_t i = 0; i < stored.realtime_targets.len; i++) {
            const trafiklab_target_t *target = &stored.realtime_targets.list[i];
            ESP_LOGI(TAG, "    %u. %s %s %s from %s%s%s (%s)", (unsigned)(i + 1),
                     target->departure.mode, target->departure.line, target->departure.destination,
                     target->stop.name, target->stop.town[0] != '\0' ? ", " : "", target->stop.town,
                     target->stop.id);
        }
    }

    stored_copy_free(&stored);
}

esp_err_t gtg_dev_config_factory_reset(void)
{
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_all(nvs);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }

    /* the runtime configuration is cleared either way, the room for the targets stays */
    trafiklab_target_list_t targets = s_config.realtime_targets;
    memset(targets.list, 0, targets.size * sizeof(targets.list[0]));
    targets.len = 0;

    memset(&s_config, 0, sizeof(s_config));
    s_config.realtime_targets = targets;

    xSemaphoreGive(s_mutex);

    if (err == ESP_OK) {
        ESP_LOGW(TAG, "factory reset, the stored configuration is erased");
    } else {
        ESP_LOGE(TAG, "factory reset failed to erase the stored configuration: %s", esp_err_to_name(err));
    }

    return err;
}
