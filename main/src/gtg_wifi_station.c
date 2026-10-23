#include "gtg_wifi_station.h"

#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_check.h"

#define WIFI_STA_MAXIMUM_RETRY     5

#define WIFI_STA_SSID_MAX_LEN      32
#define WIFI_STA_PASS_MIN_LEN      8
#define WIFI_STA_PASS_MAX_LEN      63

/* Weakest auth mode accepted when scanning. WPA3 is still negotiated when the AP supports it */
#define WIFI_STA_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "wifi station";

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_event_handler_instance_t s_wifi_event_instance = NULL;
static esp_event_handler_instance_t s_ip_event_instance = NULL;
static int s_retry_num = 0;
static bool s_wifi_initialized = false;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "disconnected from AP, reason=%d", event->reason);

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_retry_num < WIFI_STA_MAXIMUM_RETRY) {
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP (%d/%d)", s_retry_num, WIFI_STA_MAXIMUM_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t validate_credentials(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strnlen(ssid, WIFI_STA_SSID_MAX_LEN + 1);
    size_t pass_len = strnlen(pass, WIFI_STA_PASS_MAX_LEN + 1);

    if (ssid_len == 0 || ssid_len > WIFI_STA_SSID_MAX_LEN) {
        ESP_LOGE(TAG, "SSID must be 1-%d characters", WIFI_STA_SSID_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    if (pass_len < WIFI_STA_PASS_MIN_LEN || pass_len > WIFI_STA_PASS_MAX_LEN) {
        ESP_LOGE(TAG, "password must be %d-%d characters", WIFI_STA_PASS_MIN_LEN, WIFI_STA_PASS_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t wifi_station_start(const char *ssid, const char *pass)
{
    esp_err_t err;

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    s_wifi_initialized = true;

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &s_wifi_event_instance),
                        TAG, "failed to register WIFI_EVENT handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            &s_ip_event_instance),
                        TAG, "failed to register IP_EVENT handler");

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_STA_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };

    /* ssid and password are not NUL terminated when they use the full field length */
    memcpy(wifi_config.sta.ssid, ssid, strnlen(ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password, pass, strnlen(pass, sizeof(wifi_config.sta.password)));

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    /* don't leave the password lying around on the stack, the driver keeps its own copy */
    memset(&wifi_config, 0, sizeof(wifi_config));

    ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_set_config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");

    ESP_LOGI(TAG, "wifi_station_init finished, connecting to SSID:%s", ssid);

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by wifi_event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to AP SSID:%s", ssid);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "failed to connect to SSID:%s", ssid);
    return ESP_FAIL;
}

esp_err_t wifi_station_init(const char *ssid, const char *pass)
{
    if (s_sta_netif != NULL) {
        ESP_LOGE(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(validate_credentials(ssid, pass), TAG, "invalid credentials");

    s_retry_num = 0;

    esp_err_t err = wifi_station_start(ssid, pass);
    if (err != ESP_OK) {
        wifi_station_deinit();
    }

    return err;
}

void wifi_station_deinit(void) {
    /* unregister first so the disconnect caused by stopping doesn't trigger a reconnect */
    if (s_ip_event_instance != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_event_instance);
        s_ip_event_instance = NULL;
    }

    if (s_wifi_event_instance != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_instance);
        s_wifi_event_instance = NULL;
    }

    if (s_wifi_initialized) {
        /* disconnect/stop return an error if not connected/started, which is fine here */
        esp_wifi_disconnect();
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_initialized = false;
    }

    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_wifi_event_group != NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_retry_num = 0;
}
