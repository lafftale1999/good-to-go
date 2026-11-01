#include "gtg_mdns.h"

#include <stdbool.h>
#include "esp_log.h"
#include "esp_check.h"
#include "mdns.h"

#define MDNS_HOSTNAME        "goodtogo"
#define MDNS_INSTANCE_NAME   "Good to go"
#define MDNS_HTTPS_PORT      443

static const char *TAG = "mdns";

static bool s_initialized = false;

esp_err_t gtg_mdns_init(void)
{
    if (s_initialized) {
        ESP_LOGE(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init failed");
    s_initialized = true;

    esp_err_t err = mdns_hostname_set(MDNS_HOSTNAME);
    if (err == ESP_OK) {
        err = mdns_instance_name_set(MDNS_INSTANCE_NAME);
    }
    if (err == ESP_OK) {
        err = mdns_service_add(NULL, "_https", "_tcp", MDNS_HTTPS_PORT, NULL, 0);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to configure mDNS: %s", esp_err_to_name(err));
        gtg_mdns_deinit();
        return err;
    }

    ESP_LOGI(TAG, "responding to %s.local", MDNS_HOSTNAME);

    return ESP_OK;
}

void gtg_mdns_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    mdns_free();
    s_initialized = false;
}
