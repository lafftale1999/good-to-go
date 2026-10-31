#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "gtg_soft_access_point.h"
#include "gtg_wifi_station.h"
#include "gtg_ntp.h"
#include "gtg_https_server.h"
#include "gtg_mdns.h"
#include "gtg_api_keys.h"

static const char *TAG = "main";

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_ap_meta_t ap_cred;
    wifi_softap_init(&ap_cred);
    ESP_ERROR_CHECK(wifi_station_init());
    ESP_ERROR_CHECK(gtg_api_keys_init());

    ESP_ERROR_CHECK(gtg_mdns_init());
    ESP_ERROR_CHECK(gtg_https_server_init());

    ESP_ERROR_CHECK(wifi_softap_wait_for_client(pdMS_TO_TICKS(60000)));

    /* the user enters the network credentials at goodtogo.local, which connects the station */
    ESP_ERROR_CHECK(wifi_station_wait_connected(portMAX_DELAY));

    /* the keys are verified against the services, which needs the station's internet access */
    ESP_ERROR_CHECK(gtg_api_keys_wait_configured(portMAX_DELAY));

    /* The softAP stays up while the user picks stops at goodtogo.local. It is shut down
     * once the configuration has a final step to wait for */

    ESP_ERROR_CHECK(gtg_ntp_init());

    if (gtg_ntp_wait_for_sync(30000) == ESP_OK) {
        char time_str[32];
        if (gtg_ntp_get_local_time_str(time_str, sizeof(time_str)) == ESP_OK) {
            ESP_LOGI(TAG, "Swedish time: %s", time_str);
        }
    }
}
