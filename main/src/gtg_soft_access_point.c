#include "gtg_soft_access_point.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_random.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#define ESP_WIFI_CHANNEL   1
#define MAX_STA_CONN       1

#define WIFI_AP_SSID_START         "GTG"

#if CONFIG_ESP_GTK_REKEYING_ENABLE
#define GTK_REKEY_INTERVAL CONFIG_ESP_GTK_REKEY_INTERVAL
#else
#define GTK_REKEY_INTERVAL 0
#endif

static const char *TAG = "wifi softAP";

static esp_netif_t *s_ap_netif = NULL;
static esp_event_handler_instance_t s_wifi_event_instance = NULL;

static void generate_credentials(wifi_ap_meta_t *wifi_ap_meta) {
    char *pos;
    size_t tmp_size;

    // generate random SSID
    tmp_size = sizeof(WIFI_AP_SSID_START);

    snprintf(wifi_ap_meta->ssid, tmp_size, "%s", WIFI_AP_SSID_START);
    pos = &wifi_ap_meta->ssid[0] + tmp_size - 1;
    
    tmp_size = sizeof(wifi_ap_meta->ssid) - tmp_size;
    
    for (size_t idx = 0; idx < tmp_size; idx++) {
        *pos = '0' + (char)(esp_random() % 10);
        pos++;
    }

    wifi_ap_meta->ssid[8] = '\0';

    // generate random PASS
    tmp_size = sizeof(wifi_ap_meta->pass) - 1;
    pos = &wifi_ap_meta->pass[0];

    for (size_t idx = 0; idx < tmp_size; idx++) {
        *pos = '0' + (char)(esp_random() % 10);
        pos++;
    }

    wifi_ap_meta->pass[8] = '\0';
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    }
}

void wifi_softap_init(wifi_ap_meta_t *wifi_ap_meta)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &s_wifi_event_instance));

    generate_credentials(wifi_ap_meta);
    
    wifi_config_t wifi_config = {
        .ap = {
            .channel = ESP_WIFI_CHANNEL,
            .max_connection = MAX_STA_CONN,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
            .authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else /* CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT */
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
            .pmf_cfg = {
                    .required = true,
            },
#ifdef CONFIG_ESP_WIFI_BSS_MAX_IDLE_SUPPORT
            .bss_max_idle_cfg = {
                .period = WIFI_AP_DEFAULT_MAX_IDLE_PERIOD,
                .protected_keep_alive = 1,
            },
#endif
            .gtk_rekey_interval = GTK_REKEY_INTERVAL,
        },
    };

    size_t ssid_len = strlen(wifi_ap_meta->ssid);
    memcpy(wifi_config.ap.ssid, wifi_ap_meta->ssid, ssid_len);
    wifi_config.ap.ssid_len = ssid_len;
    memcpy(wifi_config.ap.password, wifi_ap_meta->pass, strlen(wifi_ap_meta->pass));

    if (strlen(wifi_ap_meta->pass) == 0 && wifi_config.ap.authmode != WIFI_AUTH_OWE) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_softap_init finished. SSID:%s password:%s channel:%d",
             wifi_ap_meta->ssid, wifi_ap_meta->pass, ESP_WIFI_CHANNEL);
}

void wifi_softap_deinit(void) {
    ESP_ERROR_CHECK(esp_wifi_stop());

    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT,
                                                          ESP_EVENT_ANY_ID,
                                                          s_wifi_event_instance));
    s_wifi_event_instance = NULL;

    ESP_ERROR_CHECK(esp_wifi_deinit());

    esp_netif_destroy_default_wifi(s_ap_netif);
    s_ap_netif = NULL;
}