#include "gtg_app.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "gtg_cache.h"
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
#define EVENT_TASK_TIMEOUT_MS   300000

/* room for the NVS erase and LCD writes of a factory reset */
#define EVENT_TASK_STACK_SIZE   4096
#define EVENT_TASK_PRIORITY     5

/* Watches the configuration while it runs, see setup_task() */
#define SETUP_TASK_STACK_SIZE   4096
#define SETUP_TASK_PRIORITY     4
#define SETUP_CHECK_PERIOD_MS   1000
#define SETUP_IDLE_TIMEOUT_US   (5LL * 60 * 1000 * 1000)

/* how long the reason a configured start failed is shown before the configuration starts */
#define ERROR_SHOW_TIME_S       3
#define AP_CLIENT_POLL_MS       500

/* lets the factory reset message be read before the restart */
#define FACTORY_RESET_RESTART_DELAY_MS  2000

#define GTG_CONFIG_URL          "goodtogo.local"

/* The definition behind gtg_app_handle_t, only visible in this file */
struct gtg_app {
    wifi_ap_meta_t          ap_cred;
    gtg_lcd_module_t        lcd;
    gtg_departure_cache_t   cache;
    size_t                  index;
    TaskHandle_t            event_task;
    bool                    ntp_started;
};

static const char *TAG = "app";

typedef struct _flag_msg_ {
    gtg_dev_flag_t flag;
    const char *msg;
} flag_msg_t;

static const flag_msg_t flag_error_msg_map[] = {
    { .flag = GTG_DEV_FLAG_CONNECTED,       .msg = "No connection..." },
    { .flag = GTG_DEV_FLAG_TIME_SYNCED,     .msg = "Time unsynched" },
    { .flag = GTG_DEV_FLAG_KEYS_VERIFIED,   .msg = "API keys invalid" },
    { .flag = GTG_DEV_FLAG_HAS_DEPARTURES,  .msg = "No stops added" }
};

static const size_t msg_map_size = sizeof(flag_error_msg_map) / sizeof(flag_error_msg_map[0]);

/* Shows the first of the missing flags in flag, and waits so it can be read */
static void print_error_state_with_delay(gtg_app_handle_t app, uint32_t flag, int32_t timeout_s) {
    for (size_t i = 0; i < msg_map_size; i++) {
        if (flag_error_msg_map[i].flag & flag) {
            gtg_lcd_print(&app->lcd, "Error:", flag_error_msg_map[i].msg);
            vTaskDelay(pdMS_TO_TICKS(timeout_s * 1000));
            return;
        }
    }

    gtg_lcd_print(&app->lcd, "Error:", "Unexpected error");
    vTaskDelay(pdMS_TO_TICKS(timeout_s * 1000));
}

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
    case GTG_EV_CONFIG_APPLIED:
        ESP_LOGI(TAG, "event: %s", gtg_event_type_name(event->type));
        break;
    default:
        ESP_LOGW(TAG, "event: unknown type %d", event->type);
        break;
    }
}

/* Erases the stored configuration and restarts, which brings the device back to its first boot
 * and into the configuration. On a failed erase the device keeps running, a restart would only
 * load the old configuration again */
static void factory_reset(gtg_app_handle_t app)
{
    ESP_LOGW(TAG, "factory reset from the button");

    if (gtg_dev_config_factory_reset() != ESP_OK) {
        gtg_lcd_print(&app->lcd, "Factory reset", "failed, retry");
        vTaskDelay(pdMS_TO_TICKS(ERROR_SHOW_TIME_S * 1000));
        return;
    }

    gtg_lcd_print(&app->lcd, "Factory reset", "Restarting...");
    vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_RESTART_DELAY_MS));
    esp_restart();
}

/* Consumes the events while gtg_app_init() runs, so the queue doesn't fill up. gtg_app_run()
 * stops it and takes over the queue, as the events would otherwise be split between them.
 * The long hold factory resets here too, so a device stuck in the configuration can be reset */
static void event_task(void *arg)
{
    gtg_app_handle_t app = arg;
    gtg_event_t event;

    while (true) {
        if (gtg_events_receive(&event, portMAX_DELAY) == ESP_OK) {
            log_event(&event);

            if (event.type == GTG_EV_BUTTON_LONG_HOLD) {
                factory_reset(app);
            }
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
        ESP_LOGI(TAG, "no WiFi stored");
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

/* SNTP is started once, by whichever path gets connected first */
static void start_ntp(gtg_app_handle_t app)
{
    if (!app->ntp_started) {
        ESP_ERROR_CHECK(gtg_ntp_init());
        app->ntp_started = true;
    }
}

/* Waits for as long as it takes, the departures can't be shown without the time. SNTP keeps
 * retrying by itself */
static void wait_for_time(gtg_app_handle_t app)
{
    if (gtg_dev_config_get_flags() & GTG_DEV_FLAG_TIME_SYNCED) {
        return;
    }

    gtg_lcd_print(&app->lcd, "Syncing time...", "");
    while (gtg_ntp_wait_for_sync(NTP_SYNC_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "still waiting for the time to sync");
    }

    char time_str[32];
    if (gtg_ntp_get_local_time_str(time_str, sizeof(time_str)) == ESP_OK) {
        ESP_LOGI(TAG, "Swedish time: %s", time_str);
    }
}

/*
 * Starts with the stored configuration, without the configuration server:
 *   1. the stored WiFi connects
 *   2. the clock syncs, waited for however long it takes
 *   3. the stored keys verify
 *   4. departures are stored
 * GTG_DEV_FLAG_VALID_CONFIG follows 1, 3 and 4 by itself, see gtg_dev_config_set_flag().
 *
 * \return ESP_OK         the configuration is valid
 * \return ESP_FAIL       the WiFi didn't connect
 * \return ESP_ERR_INVALID_STATE a key or the departures are missing
 */
static esp_err_t try_full_init(gtg_app_handle_t app) {
    if (!connect_stored_wifi()) {
        return ESP_FAIL;
    }

    start_ntp(app);
    wait_for_time(app);

    verify_stored_keys();

    return (gtg_dev_config_get_flags() & GTG_DEV_FLAG_VALID_CONFIG) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/*
 * Runs alongside the configuration server and stops itself with it:
 *  - the AP is stopped once its last client leaves and the station is connected, the user then
 *    reaches goodtogo.local over the home network
 *  - after SETUP_IDLE_TIMEOUT_US without a request the device restarts if it still isn't
 *    configured, or stops the server, mDNS and the AP once it is
 */
static void setup_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SETUP_CHECK_PERIOD_MS));

        uint32_t flags = gtg_dev_config_get_flags();

        if (wifi_softap_is_running() && !wifi_softap_has_client() && (flags & GTG_DEV_FLAG_CONNECTED)) {
            ESP_LOGI(TAG, "the client left the AP and the device is connected, stopping the AP");
            wifi_softap_deinit();
        }

        if (esp_timer_get_time() - gtg_https_server_last_activity_us() < SETUP_IDLE_TIMEOUT_US) {
            continue;
        }

        if (!(flags & GTG_DEV_FLAG_VALID_CONFIG)) {
            /* TODO: deep sleep instead, woken by the button */
            ESP_LOGW(TAG, "not configured and the configuration was idle for 5 minutes, restarting");
            esp_restart();
        }

        ESP_LOGI(TAG, "configured and the configuration was idle for 5 minutes, stopping it");
        if (wifi_softap_is_running()) {
            wifi_softap_deinit();
        }
        gtg_https_server_deinit();
        gtg_mdns_deinit();
        break;
    }

    vTaskDelete(NULL);
}

/*
 * Starts the configuration at goodtogo.local and waits until the configuration is valid. The AP
 * is only started when the station isn't connected. setup_task() restarts the device if the
 * configuration is left idle.
 */
static void run_configuration(gtg_app_handle_t app)
{
    char top_msg[17];
    char bot_msg[17];

    /* the server and mDNS answer on every interface, so the page is also reachable at
     * goodtogo.local on the user's network once the station is connected */
    ESP_ERROR_CHECK(gtg_mdns_init());
    ESP_ERROR_CHECK(gtg_https_server_init());

    if (xTaskCreate(setup_task, "gtg setup", SETUP_TASK_STACK_SIZE, NULL, SETUP_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    if (!(gtg_dev_config_get_flags() & GTG_DEV_FLAG_CONNECTED)) {
        wifi_softap_init(&app->ap_cred);
        snprintf(top_msg, sizeof(top_msg), "WIFI: %s", app->ap_cred.ssid);
        snprintf(bot_msg, sizeof(bot_msg), "PASS: %s", app->ap_cred.pass);
        gtg_lcd_print(&app->lcd, top_msg, bot_msg);

        /* polled, as setup_task() may stop the AP meanwhile */
        while (!wifi_softap_has_client() && !(gtg_dev_config_get_flags() & GTG_DEV_FLAG_CONNECTED)) {
            vTaskDelay(pdMS_TO_TICKS(AP_CLIENT_POLL_MS));
        }
    }

    ESP_LOGI(TAG, "waiting to be configured at https://%s", GTG_CONFIG_URL);
    gtg_lcd_print(&app->lcd, "Surf to webpage:", GTG_CONFIG_URL);

    /* the user enters the network credentials at goodtogo.local, which connects the station */
    ESP_ERROR_CHECK(gtg_dev_config_wait_flags(GTG_DEV_FLAG_CONNECTED, portMAX_DELAY));

    /* the departure search needs the clock, which syncs while the keys are added */
    start_ntp(app);

    /* stored keys that couldn't be verified at boot, e.g. without WiFi */
    verify_stored_keys();

    ESP_ERROR_CHECK(gtg_dev_config_wait_flags(GTG_DEV_FLAG_VALID_CONFIG, portMAX_DELAY));
    ESP_LOGI(TAG, "configured");

    wait_for_time(app);
}

esp_err_t gtg_app_init(gtg_app_handle_t *out_app)
{
    if (out_app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* zeroed, so every member starts out empty */
    gtg_app_handle_t app = calloc(1, sizeof(*app));
    if (app == NULL) {
        return ESP_ERR_NO_MEM;
    }

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
    if (xTaskCreate(event_task, "gtg events", EVENT_TASK_STACK_SIZE, app, EVENT_TASK_PRIORITY,
                    &app->event_task) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    /* the device works without the button, so a failure is only logged */
    if (gtg_button_init() != ESP_OK) {
        ESP_LOGW(TAG, "button unavailable");
    }

    /* the device works without the screen, so a missing one is only logged */
    if (gtg_lcd_init(&app->lcd) == ESP_OK) {
        gtg_lcd_print(&app->lcd, "Configuring...", "Soon good to go!");
    }

    ESP_ERROR_CHECK(gtg_dev_config_init());
    ESP_ERROR_CHECK(wifi_station_init());
    ESP_ERROR_CHECK(gtg_api_keys_init());

    /* a device with a valid stored configuration starts without the configuration */
    ret = gtg_dev_config_load();
    if (ret == ESP_OK) {
        ret = try_full_init(app);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "started with the stored configuration");
        } else {
            uint32_t missing = ~gtg_dev_config_get_flags();
            ESP_LOGW(TAG, "the stored configuration isn't valid, starting the configuration");
            print_error_state_with_delay(app, missing, ERROR_SHOW_TIME_S);
        }
    } else {
        /* nothing stored on first boot or after a factory reset, which starts the configuration from scratch */
        ESP_LOGI(TAG, "no stored configuration (%s), starting the configuration", esp_err_to_name(ret));
    }

    if (ret != ESP_OK) {
        run_configuration(app);
    }

    app->index = 0;

    *out_app = app;

    return ESP_OK;
}

esp_err_t gtg_app_run(gtg_app_handle_t app) {
    if (app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* from here this loop consumes the events */
    if (app->event_task != NULL) {
        vTaskDelete(app->event_task);
        app->event_task = NULL;
    }

    gtg_lcd_print(&app->lcd, "U R Good To Go!", "Press to search.");

    gtg_event_t event;
    uint32_t flags;

    while (true) {
        if (gtg_events_receive(&event, pdMS_TO_TICKS(EVENT_TASK_TIMEOUT_MS)) == ESP_OK) {
            flags = gtg_dev_config_get_flags();

            switch(event.type) {
                case GTG_EV_BUTTON_CLICKED:
                    if (flags & GTG_DEV_FLAG_VALID_CONFIG) {
                        // Fetch the information for current index and increase by one.
                    } else {
                        // print error message and wait for user to initiate configuration mode.
                    }
                    break;

                case GTG_EV_BUTTON_SHORT_HOLD:
                    // start configuration mode
                    break;

                case GTG_EV_BUTTON_LONG_HOLD:
                    log_event(&event);
                    factory_reset(app);
                    continue;

                case GTG_EV_CONNECTION_CHANGED:
                    // if not corrected write error message and wait for configuration mode.
                    break;

                case GTG_EV_API_KEY_REJECTED:
                    // print error message and wait for configuration mode
                    break;

                case GTG_EV_CONFIG_APPLIED:
                    // print configuration success message and start normal mode again.
                    break;

                default:
                    break;
            }

            log_event(&event);
        }

        // timeout reached, device go to sleep.
    }
}
