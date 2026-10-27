#include "gtg_wifi.h"

#include <stdbool.h>
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "wifi";

static bool s_wifi_initialized = false;
static bool s_wifi_started = false;

static esp_err_t iface_to_mode(wifi_interface_t iface, wifi_mode_t *mode)
{
    switch (iface) {
    case WIFI_IF_STA:
        *mode = WIFI_MODE_STA;
        return ESP_OK;
    case WIFI_IF_AP:
        *mode = WIFI_MODE_AP;
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static void wifi_driver_deinit(void)
{
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }

    if (s_wifi_initialized) {
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }
}

static esp_err_t wifi_driver_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    s_wifi_initialized = true;

    /* the configs contain passwords, keep them out of the driver's NVS storage */
    esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage failed");
        wifi_driver_deinit();
    }

    return err;
}

/* WIFI_MODE_NULL stops the driver entirely, other modes are switched on the fly */
static esp_err_t wifi_set_mode(wifi_mode_t mode)
{
    if (mode == WIFI_MODE_NULL) {
        wifi_driver_deinit();
        return ESP_OK;
    }

    return esp_wifi_set_mode(mode);
}

esp_err_t gtg_wifi_enable(wifi_interface_t iface, const wifi_config_t *config)
{
    wifi_mode_t iface_mode;
    ESP_RETURN_ON_ERROR(iface_to_mode(iface, &iface_mode), TAG, "unsupported interface");

    if (!s_wifi_initialized) {
        ESP_RETURN_ON_ERROR(wifi_driver_init(), TAG, "failed to initialize driver");
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), TAG, "esp_wifi_get_mode failed");

    esp_err_t err = esp_wifi_set_mode((wifi_mode_t)(mode | iface_mode));
    if (err == ESP_OK) {
        err = esp_wifi_set_config(iface, (wifi_config_t *)config);
    }
    if (err == ESP_OK && !s_wifi_started) {
        err = esp_wifi_start();
        s_wifi_started = (err == ESP_OK);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable interface: %s", esp_err_to_name(err));
        wifi_set_mode(mode);
        return err;
    }

    return ESP_OK;
}

esp_err_t gtg_wifi_disable(wifi_interface_t iface)
{
    wifi_mode_t iface_mode;
    ESP_RETURN_ON_ERROR(iface_to_mode(iface, &iface_mode), TAG, "unsupported interface");

    if (!s_wifi_initialized) {
        return ESP_OK;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    ESP_RETURN_ON_ERROR(esp_wifi_get_mode(&mode), TAG, "esp_wifi_get_mode failed");

    if ((mode & iface_mode) == 0) {
        return ESP_OK;
    }

    return wifi_set_mode((wifi_mode_t)(mode & ~iface_mode));
}
