#include "gtg_wifi_station.h"
#include "gtg_wifi.h"

#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_check.h"

#include "gtg_dev_config.h"

#define WIFI_STA_MAXIMUM_RETRY     5

#define WIFI_STA_SSID_MAX_LEN      32
#define WIFI_STA_PASS_MIN_LEN      8
#define WIFI_STA_PASS_MAX_LEN      63

/* Weakest auth mode accepted when scanning. WPA3 is still negotiated when the AP supports it */
#define WIFI_STA_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK

/* Set while the station is connected with an IP, wifi_station_wait_connected() waits for it */
#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "wifi station";

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_event_handler_instance_t s_wifi_event_instance = NULL;
static esp_event_handler_instance_t s_ip_event_instance = NULL;
static bool s_sta_enabled = false;

/* written by the event loop task, read by e.g. the HTTPS server */
static volatile wifi_station_state_t s_state = WIFI_STATION_IDLE;
static volatile uint8_t s_last_reason = 0;
static int s_retry_num = 0;

/* Stores the credentials the driver connected with in the device configuration, so only
 * credentials that work end up there */
static void store_connected_credentials(void)
{
    wifi_config_t wifi_config;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to read the connected credentials");
        return;
    }

    /* ssid and password are not NUL terminated when they use the full field length */
    char ssid[GTG_DEV_CONFIG_SSID_MAX_LEN + 1] = { 0 };
    char pass[GTG_DEV_CONFIG_PASS_MAX_LEN + 1] = { 0 };
    memcpy(ssid, wifi_config.sta.ssid, strnlen((const char *)wifi_config.sta.ssid, sizeof(ssid) - 1));
    memcpy(pass, wifi_config.sta.password, strnlen((const char *)wifi_config.sta.password, sizeof(pass) - 1));

    if (gtg_dev_config_set_wifi(ssid, pass) != ESP_OK) {
        ESP_LOGE(TAG, "failed to store the connected credentials");
    }

    memset(&wifi_config, 0, sizeof(wifi_config));
    memset(pass, 0, sizeof(pass));
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "disconnected from AP, reason=%d", event->reason);

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        /* disconnects we asked for ourselves, or after giving up, are not retried */
        if (s_state == WIFI_STATION_IDLE || s_state == WIFI_STATION_FAILED) {
            return;
        }

        s_last_reason = event->reason;

        if (s_retry_num < WIFI_STA_MAXIMUM_RETRY) {
            s_retry_num++;
            s_state = WIFI_STATION_CONNECTING;
            ESP_LOGI(TAG, "retry to connect to the AP (%d/%d)", s_retry_num, WIFI_STA_MAXIMUM_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "failed to connect, reason=%d", event->reason);
            s_state = WIFI_STATION_FAILED;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_last_reason = 0;

        /* before the state changes, so the configuration has them once the connection is reported */
        store_connected_credentials();

        s_state = WIFI_STATION_CONNECTED;
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

static void build_config(wifi_config_t *wifi_config)
{
    *wifi_config = (wifi_config_t) {
        .sta = {
            .threshold.authmode = WIFI_STA_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
}

static esp_err_t wifi_station_start(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                            WIFI_EVENT_STA_DISCONNECTED,
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

    wifi_config_t wifi_config;
    build_config(&wifi_config);

    ESP_RETURN_ON_ERROR(gtg_wifi_enable(WIFI_IF_STA, &wifi_config), TAG, "failed to enable station");
    s_sta_enabled = true;

    ESP_LOGI(TAG, "wifi_station_init finished");

    return ESP_OK;
}

esp_err_t wifi_station_init(void)
{
    if (s_sta_netif != NULL) {
        ESP_LOGE(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = wifi_station_start();
    if (err != ESP_OK) {
        wifi_station_deinit();
    }

    return err;
}

esp_err_t wifi_station_connect(const char *ssid, const char *pass)
{
    esp_err_t err;

    if (!s_sta_enabled) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state == WIFI_STATION_CONNECTING) {
        ESP_LOGE(TAG, "already connecting");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(validate_credentials(ssid, pass), TAG, "invalid credentials");

    /* IDLE first so the disconnect from a previous network isn't retried */
    s_state = WIFI_STATION_IDLE;
    esp_wifi_disconnect();

    wifi_config_t wifi_config;
    build_config(&wifi_config);

    /* ssid and password are not NUL terminated when they use the full field length */
    memcpy(wifi_config.sta.ssid, ssid, strnlen(ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password, pass, strnlen(pass, sizeof(wifi_config.sta.password)));

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    /* don't leave the password lying around on the stack, the driver keeps its own copy */
    memset(&wifi_config, 0, sizeof(wifi_config));

    ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_set_config failed");

    s_retry_num = 0;
    s_last_reason = 0;
    s_state = WIFI_STATION_CONNECTING;

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        s_state = WIFI_STATION_FAILED;
        return err;
    }

    ESP_LOGI(TAG, "connecting to SSID:%s", ssid);

    return ESP_OK;
}

/* Keeps results sorted by RSSI with one entry per SSID, the weakest entry drops out when full */
static void insert_scan_result(wifi_station_scan_result_t *results, size_t max_results,
                               size_t *count, const wifi_ap_record_t *record)
{
    const char *ssid = (const char *)record->ssid;
    if (ssid[0] == '\0') {
        return;
    }

    for (size_t i = 0; i < *count; i++) {
        if (strncmp(results[i].ssid, ssid, sizeof(results[i].ssid)) == 0) {
            if (results[i].rssi >= record->rssi) {
                return;
            }
            /* the stronger one is inserted below */
            memmove(&results[i], &results[i + 1], (*count - i - 1) * sizeof(*results));
            (*count)--;
            break;
        }
    }

    size_t pos = 0;
    while (pos < *count && results[pos].rssi >= record->rssi) {
        pos++;
    }
    if (pos >= max_results) {
        return;
    }

    size_t moved = (*count < max_results ? *count : max_results - 1) - pos;
    memmove(&results[pos + 1], &results[pos], moved * sizeof(*results));
    if (*count < max_results) {
        (*count)++;
    }

    strlcpy(results[pos].ssid, ssid, sizeof(results[pos].ssid));
    results[pos].rssi = record->rssi;
    results[pos].password_protected = record->authmode != WIFI_AUTH_OPEN
                                      && record->authmode != WIFI_AUTH_OWE;
}

esp_err_t wifi_station_scan(wifi_station_scan_result_t *results, size_t max_results, size_t *count)
{
    if (results == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    if (!s_sta_enabled) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* the driver refuses to scan while connecting */
    if (s_state == WIFI_STATION_CONNECTING) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_ERR_WIFI_STATE) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_scan_start failed");

    /* fetching one record at a time frees each one, avoiding a buffer for the whole list */
    wifi_ap_record_t record;
    while (esp_wifi_scan_get_ap_record(&record) == ESP_OK) {
        insert_scan_result(results, max_results, count, &record);
    }
    esp_wifi_clear_ap_list();

    ESP_LOGI(TAG, "scan found %u networks", (unsigned)*count);

    return ESP_OK;
}

wifi_station_state_t wifi_station_get_state(uint8_t *reason)
{
    if (reason != NULL) {
        *reason = s_last_reason;
    }

    return s_state;
}

esp_err_t wifi_station_wait_connected(TickType_t ticks)
{
    if (s_wifi_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            ticks);

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void wifi_station_deinit(void) {
    /* unregister first so the disconnect caused by stopping doesn't trigger a reconnect */
    if (s_ip_event_instance != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_event_instance);
        s_ip_event_instance = NULL;
    }

    if (s_wifi_event_instance != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_wifi_event_instance);
        s_wifi_event_instance = NULL;
    }

    if (s_sta_enabled) {
        /* disconnect returns an error if not connected, which is fine here */
        esp_wifi_disconnect();
        gtg_wifi_disable(WIFI_IF_STA);
        s_sta_enabled = false;
    }

    if (s_sta_netif != NULL) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_wifi_event_group != NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_state = WIFI_STATION_IDLE;
    s_last_reason = 0;
    s_retry_num = 0;
}
