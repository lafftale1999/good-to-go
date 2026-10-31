#include <stdio.h>
#include <string.h>

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
#include "gtg_dev_config.h"
#include "gtg_lcd.h"
#include "gtg_events.h"
#include "gtg_button.h"

/* the station retries 5 times, which ends well within this */
#define BOOT_WIFI_TIMEOUT_MS    60000
#define NTP_SYNC_TIMEOUT_MS     30000

#define EVENT_TASK_STACK_SIZE   3072
#define EVENT_TASK_PRIORITY     5

static const char *TAG = "main";

static gtg_lcd_module_t s_lcd;

static void log_event(const gtg_event_t *event)
{
    switch (event->type) {
    case GTG_EV_CONNECTION_CHANGED:
        ESP_LOGI(TAG, "event: %s, %s (reason %u)", gtg_event_type_name(event->type),
                 event->wifi.connected ? "connected" : "disconnected", event->wifi.reason);
        break;
    case GTG_EV_API_KEY_REJECTED:
        ESP_LOGI(TAG, "event: %s, %s", gtg_event_type_name(event->type),
                 event->api_key.which == GTG_API_KEY_TRAFIKLAB ? "Trafiklab" : "Google");
        break;
    case GTG_EV_BUTTON_CLICKED:
    case GTG_EV_BUTTON_SHORT_HOLD:
    case GTG_EV_BUTTON_LONG_HOLD:
        ESP_LOGI(TAG, "event: %s, button %u", gtg_event_type_name(event->type), event->button.id);
        break;
    default:
        ESP_LOGW(TAG, "event: unknown type %d", event->type);
        break;
    }
}

/* Consumes the events the modules post. It runs in its own task, as the main task blocks while
 * waiting for the configuration. The events are only logged for now, this is where the device's
 * reactions to them go */
static void event_task(void *arg)
{
    gtg_event_t event;

    while (true) {
        if (gtg_events_receive(&event, portMAX_DELAY) == ESP_OK) {
            log_event(&event);
        }
    }
}

/* Connects with the stored credentials. False when none are stored or the retries fail */
static bool connect_stored_wifi(void)
{
    char ssid[GTG_DEV_CONFIG_SSID_MAX_LEN + 1];
    char pass[GTG_DEV_CONFIG_PASS_MAX_LEN + 1];

    esp_err_t err = gtg_dev_config_get_wifi(ssid, pass);
    if (err == ESP_OK) {
        err = wifi_station_connect(ssid, pass);
    }
    memset(pass, 0, sizeof(pass));

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "no WiFi stored, starting configuration");
        return false;
    }
    if (err == ESP_OK) {
        err = wifi_station_wait_result(pdMS_TO_TICKS(BOOT_WIFI_TIMEOUT_MS));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not connect to the stored WiFi SSID:%s: %s", ssid, esp_err_to_name(err));
        return false;
    }

    return true;
}

/* Verifies each stored key that isn't verified yet, e.g. by the user meanwhile. A key that
 * fails stays stored, the user is asked for it on the key step */
static void verify_stored_keys(void)
{
    for (gtg_api_key_t which = 0; which < GTG_API_KEY_COUNT; which++) {
        if (gtg_api_keys_is_set(which)) {
            continue;
        }

        char key[GTG_API_KEY_MAX_LEN + 1];
        if (gtg_dev_config_get_api_key(which, key, sizeof(key)) == ESP_OK) {
            /* logs the result itself */
            gtg_api_keys_set(which, key);
        }
        memset(key, 0, sizeof(key));
    }
}

/*
 * Startup follows the configuration steps with what is stored in NVS, and the first step that
 * is missing or fails is where the configuration at goodtogo.local starts:
 *
 *   1. WiFi stored -> connect           otherwise: start the AP and configure WiFi
 *   2. connected   -> sync the clock
 *   3. keys stored -> verify them       otherwise: configure the keys
 *   4. departures loaded                the departure step is shown either way
 *
 * The steps are tracked in the gtg_dev_flag_t flags of the device configuration, which the
 * page reads from /api/status to start at the right step.
 */
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

    /* before anything that posts, so even the first connection is seen */
    ESP_ERROR_CHECK(gtg_events_init());
    if (xTaskCreate(event_task, "gtg events", EVENT_TASK_STACK_SIZE, NULL, EVENT_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    /* the device works without the button, so a failure is only logged */
    if (gtg_button_init() != ESP_OK) {
        ESP_LOGW(TAG, "button unavailable");
    }

    /* the device works without the screen, so a missing one is only logged */
    if (gtg_lcd_init(&s_lcd) == ESP_OK) {
        gtg_lcd_print(&s_lcd, "Hello world", NULL);
    }

    ESP_ERROR_CHECK(gtg_dev_config_init());

    /* nothing stored on first boot or after a factory reset, which starts the configuration from scratch */
    ret = gtg_dev_config_load();
    if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored configuration");
    }

    ESP_ERROR_CHECK(wifi_station_init());
    ESP_ERROR_CHECK(gtg_api_keys_init());

    /* the server and mDNS answer on every interface, so the page is also reachable at
     * goodtogo.local on the user's network once the station is connected */
    ESP_ERROR_CHECK(gtg_mdns_init());
    ESP_ERROR_CHECK(gtg_https_server_init());

    /* 1. WiFi */
    if (!connect_stored_wifi()) {
        wifi_ap_meta_t ap_cred;
        wifi_softap_init(&ap_cred);

        /* the user enters the network credentials at goodtogo.local, which connects the station */
        ESP_ERROR_CHECK(wifi_station_wait_connected(portMAX_DELAY));
    }

    /* 2. the clock, which the departure search needs. The keys can be added meanwhile */
    ESP_ERROR_CHECK(gtg_ntp_init());

    if (gtg_ntp_wait_for_sync(NTP_SYNC_TIMEOUT_MS) == ESP_OK) {
        char time_str[32];
        if (gtg_ntp_get_local_time_str(time_str, sizeof(time_str)) == ESP_OK) {
            ESP_LOGI(TAG, "Swedish time: %s", time_str);
        }
    }

    /* 3. the API keys, verified against the services */
    verify_stored_keys();
    if (gtg_api_keys_wait_configured(0) != ESP_OK) {
        ESP_LOGI(TAG, "waiting for the API keys to be configured");
        ESP_ERROR_CHECK(gtg_api_keys_wait_configured(portMAX_DELAY));
    }

    /* 4. departures, loaded with the configuration and added at goodtogo.local */
    uint32_t flags = gtg_dev_config_get_flags();
    ESP_LOGI(TAG, "configured, %s", (flags & GTG_DEV_FLAG_HAS_DEPARTURES)
             ? "departures are tracked" : "no departures are tracked yet");

    /* A softAP started for the configuration stays up while the user picks departures. It is
     * shut down once the configuration has a final step to wait for */
}
