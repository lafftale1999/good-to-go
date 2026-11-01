#include "gtg_app.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

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
#include "gtg_sleep.h"

/* the station retries 5 times, which ends well within this */
#define BOOT_WIFI_TIMEOUT_MS    60000
#define NTP_SYNC_TIMEOUT_MS     30000

/* room for the NVS erase and LCD writes of a factory reset */
#define EVENT_TASK_STACK_SIZE   4096
#define EVENT_TASK_PRIORITY     5

/* Watches the activity from boot, see watch_task() */
#define WATCH_TASK_STACK_SIZE   4096
#define WATCH_TASK_PRIORITY     4
#define WATCH_PERIOD_MS         1000

/* without a request the configuration server stops, without any activity the device sleeps */
#define SERVER_IDLE_TIMEOUT_US  (5LL * 60 * 1000 * 1000)
#define SLEEP_IDLE_TIMEOUT_US   (5LL * 60 * 1000 * 1000)

/* lets the sleep message be read before the screen goes dark */
#define SLEEP_MESSAGE_MS        1500

/* how long the reason a configured start failed is shown before the configuration starts */
#define ERROR_SHOW_TIME_S       3
#define AP_CLIENT_POLL_MS       500

/* how long the AP must be without its client before the client counts as gone. When the station
 * joins a network the AP moves to its channel, which drops the client for a moment until it
 * rejoins, and that must not stop the AP under it */
#define AP_CLIENT_GONE_US       (5LL * 1000 * 1000)

/* lets the factory reset message be read before the restart */
#define FACTORY_RESET_RESTART_DELAY_MS  2000

#define GTG_CONFIG_URL          "goodtogo.local"

/* Optional factory keys, e.g. from cred/credentials.h. Each one is tried when no key is stored
 * for its service, see verify_stored_keys() */
#if __has_include("credentials.h")
#include "credentials.h"
#endif

#ifdef FACTORY_TRAFIKLAB_API_KEY
#define FACTORY_TRAFIKLAB_KEY   FACTORY_TRAFIKLAB_API_KEY
#else
#define FACTORY_TRAFIKLAB_KEY   NULL
#endif

#ifdef FACTORY_GOOGLE_API_KEY
#define FACTORY_GOOGLE_KEY      FACTORY_GOOGLE_API_KEY
#else
#define FACTORY_GOOGLE_KEY      NULL
#endif

static const char *const FACTORY_KEYS[GTG_API_KEY_COUNT] = {
    [GTG_API_KEY_TRAFIKLAB] = FACTORY_TRAFIKLAB_KEY,
    [GTG_API_KEY_GOOGLE]    = FACTORY_GOOGLE_KEY,
};

#define START_MSG_TOP           "U R Good To Go!"
#define START_MSG_BOTTOM        "Press to search."

/* shown after the last departure, before the double-click wraps around to the first */
#define END_MSG_TOP             "Need more stops?"
#define END_MSG_BOTTOM          "Hold 5 sec..."

/* shown in the configuration mode once the configuration is valid, a short hold stops it */
#define CONFIG_HINT_TOP         "Stop config mode"
#define CONFIG_HINT_BOTTOM      "Hold 5 sec..."

/* shown while the station retries after the connection was lost while running */
#define CONN_LOST_TOP           "Connection lost!"

/* departure fetches that may fail in a row before the configuration mode starts */
#define FETCH_FAILURES_MAX      3

/* The definition behind gtg_app_handle_t, only visible in this file */
struct gtg_app {
    wifi_ap_meta_t          ap_cred;
    gtg_lcd_module_t        lcd;
    gtg_departure_cache_t   cache;

    /* the chosen departure, cache.len is the end message after the last one */
    size_t                  index;

    /* while a departure is shown, it is drawn again at redraw_at for its times to count down,
     * and fetched again in the background once it is due, see on_redraw_timer() */
    bool                    redraw;
    TickType_t              redraw_at;
    TaskHandle_t            event_task;
    bool                    ntp_started;

    /* set while the station retries after the connection was lost while running, and whether
     * a departure was shown then, which is drawn again once it reconnects */
    bool                    conn_lost;
    bool                    resume_redraw;

    /* departure fetches that failed in a row, a successful one resets it */
    uint8_t                 fetch_failures;

    /* the configuration mode is started and stopped from several tasks */
    SemaphoreHandle_t       mode_lock;
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
    case GTG_EV_CONNECTION_RETRYING:
        ESP_LOGI(TAG, "event: %s, %u/%u (reason %u)", gtg_event_type_name(event->type),
                 event->wifi.retry, event->wifi.max_retries, event->wifi.reason);
        break;
    case GTG_EV_CONNECTION_FAILED:
        ESP_LOGI(TAG, "event: %s (reason %u)", gtg_event_type_name(event->type), event->wifi.reason);
        break;
    case GTG_EV_API_KEY_REJECTED:
        ESP_LOGI(TAG, "event: %s, %s", gtg_event_type_name(event->type),
                 event->api_key.which == GTG_API_KEY_TRAFIKLAB ? "Trafiklab" : "Google");
        break;
    case GTG_EV_BUTTON_CLICKED:
    case GTG_EV_BUTTON_TRIPLE_CLICKED:
    case GTG_EV_BUTTON_SHORT_HOLD:
    case GTG_EV_BUTTON_LONG_HOLD:
        ESP_LOGI(TAG, "event: %s, button %u", gtg_event_type_name(event->type), event->button.id);
        break;
    case GTG_EV_CONFIG_APPLIED:
    case GTG_EV_TARGET_ADDED:
    case GTG_EV_TARGET_REMOVED:
    case GTG_EV_CONFIG_LOGIN:
        ESP_LOGI(TAG, "event: %s", gtg_event_type_name(event->type));
        break;
    default:
        ESP_LOGW(TAG, "event: unknown type %d", event->type);
        break;
    }
}

/* The login screen of the configuration: the address and the password the page asks for, made
 * anew at every start of the server */
static void show_config_login(gtg_app_handle_t app)
{
    char pass[GTG_HTTPS_PASSWORD_LEN + 1];
    if (gtg_https_server_get_password(pass, sizeof(pass)) != ESP_OK) {
        gtg_lcd_print(&app->lcd, "Surf to webpage:", GTG_CONFIG_URL);
        return;
    }

    char bottom[sizeof("PASS: ") + GTG_HTTPS_PASSWORD_LEN];
    snprintf(bottom, sizeof(bottom), "PASS: %s", pass);
    gtg_lcd_print(&app->lcd, GTG_CONFIG_URL, bottom);

    memset(pass, 0, sizeof(pass));
    memset(bottom, 0, sizeof(bottom));
}

/* A user logged in to the configuration page, the password is no longer needed on the screen */
static void show_config_logged_in(gtg_app_handle_t app)
{
    gtg_lcd_print(&app->lcd, "Configuring...", "");
}

static esp_err_t config_mode_start(gtg_app_handle_t app);

/* A network chosen in the configuration couldn't be joined. The page may have been reached over
 * the old network, which the device has left, so its own network starts to stay reachable, and
 * its name and password are shown. Nothing to do without the configuration, or with the AP on */
static void on_config_connection_failed(gtg_app_handle_t app)
{
    if (!gtg_https_server_is_running() || wifi_softap_is_running()) {
        return;
    }

    ESP_LOGW(TAG, "could not join the chosen network, starting the device's own network");
    config_mode_start(app);
}

/* Whether the event is one the configuration takes: anything but a click or a triple click. In
 * the configuration only the short hold, which leaves it, and the long hold, which factory
 * resets, act on the button */
static bool is_config_button(gtg_event_type_t type)
{
    return type != GTG_EV_BUTTON_CLICKED && type != GTG_EV_BUTTON_TRIPLE_CLICKED;
}

/* Shows the sleep message and goes to deep sleep, from which the button wakes the device by
 * restarting it. Doesn't return */
static void go_to_sleep(gtg_app_handle_t app, const char *reason)
{
    ESP_LOGI(TAG, "%s, going to sleep", reason);

    gtg_lcd_print(&app->lcd, "Going to sleep", "Press to wake");
    vTaskDelay(pdMS_TO_TICKS(SLEEP_MESSAGE_MS));
    gtg_lcd_clear(&app->lcd);
    gtg_lcd_set_backlight(&app->lcd, false);

    gtg_sleep_start();
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
 * Until it has started the device is in the configuration, where only the holds act, see
 * is_config_button(). The configuration can't be left before it is valid, so the short hold is
 * only logged, while the long hold factory resets, so a device stuck in the configuration can be
 * reset */
static void event_task(void *arg)
{
    gtg_app_handle_t app = arg;
    gtg_event_t event;

    while (true) {
        if (gtg_events_receive(&event, portMAX_DELAY) != ESP_OK) {
            continue;
        }

        switch (event.type) {
        case GTG_EV_BUTTON_CLICKED:
        case GTG_EV_BUTTON_TRIPLE_CLICKED:
        case GTG_EV_BUTTON_SHORT_HOLD:
            ESP_LOGI(TAG, "event: %s, ignored until the device has started", gtg_event_type_name(event.type));
            break;
        case GTG_EV_BUTTON_LONG_HOLD:
            log_event(&event);
            factory_reset(app);
            break;
        case GTG_EV_CONFIG_LOGIN:
            log_event(&event);
            show_config_logged_in(app);
            break;
        case GTG_EV_CONNECTION_FAILED:
            log_event(&event);
            on_config_connection_failed(app);
            break;
        default:
            log_event(&event);
            break;
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
 * fails stays stored, the user is asked for it on the key step. When no key is stored for a
 * service its factory key is tried instead, and stored if it works */
static void verify_stored_keys(void)
{
    for (gtg_api_key_t which = 0; which < GTG_API_KEY_COUNT; which++) {
        if (gtg_api_keys_is_set(which)) {
            continue;
        }

        char key[GTG_API_KEY_MAX_LEN + 1];
        esp_err_t err = gtg_dev_config_get_api_key(which, key, sizeof(key));
        if (err == ESP_OK) {
            /* logs the result itself */
            gtg_api_keys_set(which, key);
        } else if (err == ESP_ERR_NOT_FOUND && FACTORY_KEYS[which] != NULL) {
            ESP_LOGI(TAG, "no key stored for service %d, trying the factory key", which);
            gtg_api_keys_set(which, FACTORY_KEYS[which]);
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

/* The departures can't be shown without the time, so this waits until it syncs. SNTP keeps
 * retrying by itself, and watch_task() puts the device to sleep if it takes 5 minutes */
static void wait_for_time(gtg_app_handle_t app)
{
    if (gtg_dev_config_get_flags() & GTG_DEV_FLAG_TIME_SYNCED) {
        return;
    }

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

/* The last activity of the device: a posted event, e.g. a button push or a connection change,
 * or a request to the configuration server */
static int64_t last_activity_us(void)
{
    int64_t events = gtg_events_last_activity_us();
    int64_t server = gtg_https_server_last_activity_us();

    return events > server ? events : server;
}

/* Stops the AP, the server and mDNS, whichever run. Called with mode_lock held */
static void config_mode_stop_locked(void)
{
    if (wifi_softap_is_running()) {
        wifi_softap_deinit();
    }
    if (gtg_https_server_is_running()) {
        gtg_https_server_deinit();
        gtg_mdns_deinit();
    }
}

/*
 * Starts the configuration mode: the server and mDNS, which answer on every interface, so the
 * page is reachable at goodtogo.local on the user's network once the station is connected. The
 * AP is only started when the station isn't connected, and its credentials are shown on the
 * LCD, otherwise the address and the login password are.
 */
static esp_err_t config_mode_start(gtg_app_handle_t app)
{
    esp_err_t err = ESP_OK;

    xSemaphoreTake(app->mode_lock, portMAX_DELAY);

    /* the server and mDNS are always started and stopped together */
    if (!gtg_https_server_is_running()) {
        err = gtg_mdns_init();
        if (err == ESP_OK) {
            err = gtg_https_server_init();
            if (err != ESP_OK) {
                gtg_mdns_deinit();
            }
        }
    }

    if (err == ESP_OK && !(gtg_dev_config_get_flags() & GTG_DEV_FLAG_CONNECTED) && !wifi_softap_is_running()) {
        wifi_softap_init(&app->ap_cred);
    }

    if (err == ESP_OK && wifi_softap_is_running()) {
        char top_msg[GTG_LCD_COLUMNS + 1];
        char bot_msg[GTG_LCD_COLUMNS + 1];
        snprintf(top_msg, sizeof(top_msg), "WIFI: %s", app->ap_cred.ssid);
        snprintf(bot_msg, sizeof(bot_msg), "PASS: %s", app->ap_cred.pass);
        gtg_lcd_print(&app->lcd, top_msg, bot_msg);
    } else if (err == ESP_OK) {
        show_config_login(app);
    }

    xSemaphoreGive(app->mode_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start the configuration mode: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "configuration mode on, at https://%s", GTG_CONFIG_URL);
    }

    return err;
}

static void config_mode_stop(gtg_app_handle_t app)
{
    xSemaphoreTake(app->mode_lock, portMAX_DELAY);
    config_mode_stop_locked();
    xSemaphoreGive(app->mode_lock);

    ESP_LOGI(TAG, "configuration mode off");
}

/*
 * Runs from boot, whatever the device is doing, and checks every second:
 *  - the LCD switches from the AP's credentials to goodtogo.local and the login password when the
 *    first client joins the AP
 *  - the AP is stopped once its client has left and the station is connected, the user then
 *    reaches goodtogo.local over the home network. A client counts as gone after
 *    AP_CLIENT_GONE_US without one, so the AP isn't stopped while a client rejoins after the AP
 *    moved channel. An AP that never had a client is stopped as soon as the station connects.
 *    Otherwise only leaving the configuration stops the AP
 *  - the configuration server, mDNS and the AP are stopped once the configuration is valid and
 *    the server has had no request for 5 minutes
 *  - the device goes to deep sleep after 5 minutes without any activity, see last_activity_us()
 */
static void watch_task(void *arg)
{
    gtg_app_handle_t app = arg;

    /* whether the running AP has had a client, and since when it has had none, 0 while it has
     * one. Both start over when the AP stops */
    bool ap_had_client = false;
    int64_t ap_empty_since = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(WATCH_PERIOD_MS));

        uint32_t flags = gtg_dev_config_get_flags();
        int64_t now = esp_timer_get_time();

        xSemaphoreTake(app->mode_lock, portMAX_DELAY);

        bool ap_running = wifi_softap_is_running();
        bool ap_has_client = ap_running && wifi_softap_has_client();

        if (!ap_running) {
            ap_had_client = false;
            ap_empty_since = 0;
        } else if (ap_has_client) {
            /* the AP's credentials are shown until the first client joins, then the address it
             * surfs to and the login password. A client rejoining after a moment away doesn't
             * show them again, the screen may show that it logged in */
            if (!ap_had_client) {
                show_config_login(app);
            }
            ap_had_client = true;
            ap_empty_since = 0;
        } else if (ap_empty_since == 0) {
            ap_empty_since = now;
        }

        bool client_gone = !ap_had_client || now - ap_empty_since >= AP_CLIENT_GONE_US;
        if (ap_running && !ap_has_client && client_gone && (flags & GTG_DEV_FLAG_CONNECTED)) {
            ESP_LOGI(TAG, "the AP has no client and the device is connected, stopping the AP");
            wifi_softap_deinit();
            ap_had_client = false;
            ap_empty_since = 0;
        }

        if (gtg_https_server_is_running() && (flags & GTG_DEV_FLAG_VALID_CONFIG)
            && now - gtg_https_server_last_activity_us() >= SERVER_IDLE_TIMEOUT_US) {
            ESP_LOGI(TAG, "configured and the configuration was idle for 5 minutes, stopping it");
            config_mode_stop_locked();
        }

        xSemaphoreGive(app->mode_lock);

        if (now - last_activity_us() >= SLEEP_IDLE_TIMEOUT_US) {
            go_to_sleep(app, "no activity for 5 minutes");
        }
    }
}

/*
 * Starts the configuration at goodtogo.local and waits until the configuration is valid.
 * watch_task() puts the device to sleep if the configuration is left idle.
 */
static void run_configuration(gtg_app_handle_t app)
{
    ESP_ERROR_CHECK(config_mode_start(app));

    if (wifi_softap_is_running()) {
        /* polled, as watch_task() may stop the AP meanwhile */
        while (!wifi_softap_has_client() && !(gtg_dev_config_get_flags() & GTG_DEV_FLAG_CONNECTED)) {
            vTaskDelay(pdMS_TO_TICKS(AP_CLIENT_POLL_MS));
        }
    }

    ESP_LOGI(TAG, "waiting to be configured at https://%s", GTG_CONFIG_URL);
    show_config_login(app);

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

    gtg_sleep_log_wakeup();

    app->mode_lock = xSemaphoreCreateMutex();
    if (app->mode_lock == NULL) {
        free(app);
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

    /* after the LCD, which it prints the sleep message on. The idle time counts from the event
     * queue's init, which is from boot */
    if (xTaskCreate(watch_task, "gtg watch", WATCH_TASK_STACK_SIZE, app, WATCH_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
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

    *out_app = app;

    return ESP_OK;
}

/* Copies the targets again when they were reconfigured, which clears the cache */
static void sync_cache(gtg_app_handle_t app)
{
    if (gtg_cache_sync(&app->cache)) {
        ESP_LOGI(TAG, "the departures were reconfigured, the next click shows the first");
        app->index = app->cache.len;
    }
}

/* The top row of a departure: its line and destination. A longer name than the screen is cut,
 * with a '.' as its last character */
static void format_departure_name(const trafiklab_target_t *target, char top[GTG_LCD_COLUMNS + 1])
{
    char name[GTG_DEPARTURE_LINE_MAX_LEN + GTG_DEPARTURE_DEST_MAX_LEN + 2];
    snprintf(name, sizeof(name), "%s %s", target->departure.line, target->departure.destination);

    /* converted first, so the length is in the characters the LCD shows */
    char shown[sizeof(name)];
    size_t len = gtg_lcd_to_ascii(shown, sizeof(shown), name);

    if (len > GTG_LCD_COLUMNS) {
        shown[GTG_LCD_COLUMNS - 1] = '.';
        shown[GTG_LCD_COLUMNS] = '\0';
    }
    strlcpy(top, shown, GTG_LCD_COLUMNS + 1);
}

/*
 * Draws the chosen departure from the cache, without fetching: its name on the top row and the
 * minutes to its coming departures on the bottom one, e.g. "now, 13, 28 min".
 *
 * \return ticks until a time on the screen changes, portMAX_DELAY when none will
 */
static TickType_t draw_departure(gtg_app_handle_t app)
{
    const gtg_departure_node_t *node = &app->cache.nodes[app->index];

    char top[GTG_LCD_COLUMNS + 1];
    format_departure_name(&app->cache.targets[app->index], top);

    if (node->status != ESP_OK || node->ts == 0) {
        gtg_lcd_print(&app->lcd, top, "Fetch failed");
        return portMAX_DELAY;
    }

    int32_t remaining[GTG_DEPARTURES_UPCOMING_MAX];
    size_t count = gtg_cache_remaining(&app->cache, app->index, remaining);
    if (count == 0) {
        gtg_lcd_print(&app->lcd, top, "No departures...");
        return portMAX_DELAY;
    }

    char bottom[GTG_LCD_COLUMNS + 1] = "";
    size_t used = 0;
    int32_t next_change_s = INT32_MAX;

    for (size_t i = 0; i < count; i++) {
        const char *sep = i > 0 ? ", " : "";
        int n = remaining[i] < 60
            ? snprintf(bottom + used, sizeof(bottom) - used, "%snow", sep)
            : snprintf(bottom + used, sizeof(bottom) - used, "%s%ld", sep, (long)(remaining[i] / 60));
        if (n < 0 || (size_t)n >= sizeof(bottom) - used) {
            break;
        }
        used += (size_t)n;

        /* the shown minutes drop, or "now" departs, once the remainder of the minute is gone */
        int32_t change_s = remaining[i] % 60 + 1;
        if (change_s < next_change_s) {
            next_change_s = change_s;
        }
    }

    /* "now" alone needs no unit */
    if (count > 1 || remaining[0] >= 60) {
        strlcat(bottom, " min", sizeof(bottom));
    }

    gtg_lcd_print(&app->lcd, top, bottom);

    return pdMS_TO_TICKS(next_change_s * 1000);
}

/* Stops redrawing, e.g. when the screen shows something else than a departure */
static void stop_redraw(gtg_app_handle_t app)
{
    app->redraw = false;
}

/* Shows why the configuration mode starts, then starts it */
static void enter_config_mode(gtg_app_handle_t app, const char *top, const char *bottom)
{
    stop_redraw(app);
    gtg_lcd_print(&app->lcd, top, bottom);
    vTaskDelay(pdMS_TO_TICKS(ERROR_SHOW_TIME_S * 1000));
    config_mode_start(app);
}

/*
 * Fetches the departure at index when it is due, see gtg_cache_refresh(), and counts the
 * fetches that fail in a row. At FETCH_FAILURES_MAX the configuration mode starts, unless it
 * already runs, and the count starts over. A fresh node makes no request and isn't counted.
 *
 * \return false when the configuration mode started, which then has the screen
 */
static bool refresh_departure(gtg_app_handle_t app, size_t index)
{
    if (gtg_cache_is_fresh(&app->cache, index)) {
        return true;
    }

    esp_err_t err = gtg_cache_refresh(&app->cache, index);
    if (err == ESP_OK) {
        if (app->fetch_failures > 0) {
            ESP_LOGI(TAG, "fetch succeeded, %u failures in a row cleared", app->fetch_failures);
        }
        app->fetch_failures = 0;
        return true;
    }

    app->fetch_failures++;
    ESP_LOGW(TAG, "fetch failed, %u/%u in a row: %s", app->fetch_failures, FETCH_FAILURES_MAX,
             esp_err_to_name(err));

    if (app->fetch_failures < FETCH_FAILURES_MAX) {
        return true;
    }

    app->fetch_failures = 0;

    if (gtg_https_server_is_running()) {
        ESP_LOGW(TAG, "too many failed fetches, the configuration mode already runs");
        return true;
    }

    ESP_LOGW(TAG, "too many failed fetches, starting the configuration mode");
    enter_config_mode(app, "Fetch failed 3x", "Starting config");
    return false;
}

/* Draws the chosen departure and plans the next redraw for when its times change */
static void redraw_departure(gtg_app_handle_t app)
{
    TickType_t wait = draw_departure(app);

    /* also when nothing on the screen changes, e.g. "No departures..." or "Fetch failed", the
     * next background fetch is due a minute after the last one */
    TickType_t until_due = pdMS_TO_TICKS(gtg_cache_seconds_until_due(&app->cache, app->index) * 1000);
    if (until_due < wait) {
        wait = until_due;
    }

    app->redraw = true;
    app->redraw_at = xTaskGetTickCount() + wait;
}

/* The redraw timer of the shown departure. Its information is fetched again in the background
 * once it is due, without "Updating...", the screen keeps the old times until the new ones or
 * the failure are drawn */
static void on_redraw_timer(gtg_app_handle_t app)
{
    if (gtg_dev_config_get_flags() & GTG_DEV_FLAG_VALID_CONFIG) {
        sync_cache(app);
        if (app->index >= app->cache.len) {
            /* reconfigured meanwhile, the next press shows the new departures */
            stop_redraw(app);
            return;
        }

        if (gtg_cache_seconds_until_due(&app->cache, app->index) == 0
            && !refresh_departure(app, app->index)) {
            return;
        }
    }

    redraw_departure(app);
}

/* Refreshes the chosen departure when its information is 60 s or older, shows and logs it.
 * After the last departure the end message is shown instead */
static void show_chosen(gtg_app_handle_t app)
{
    uint32_t flags = gtg_dev_config_get_flags();
    if (!(flags & GTG_DEV_FLAG_VALID_CONFIG)) {
        ESP_LOGW(TAG, "no departures can be fetched, the configuration isn't valid");
        stop_redraw(app);
        print_error_state_with_delay(app, ~flags, 0);
        return;
    }

    sync_cache(app);

    if (app->index >= app->cache.len) {
        ESP_LOGI(TAG, "after the last departure, hold 5 seconds to add more");
        stop_redraw(app);
        gtg_lcd_print(&app->lcd, END_MSG_TOP, END_MSG_BOTTOM);
        return;
    }

    if (!gtg_cache_is_fresh(&app->cache, app->index)) {
        char top[GTG_LCD_COLUMNS + 1];
        format_departure_name(&app->cache.targets[app->index], top);
        gtg_lcd_print(&app->lcd, top, "Updating...");
        if (!refresh_departure(app, app->index)) {
            return;
        }
    }

    gtg_cache_log(&app->cache, app->index);
    redraw_departure(app);
}

/* Moves to the next departure, or the end message after the last one, then wraps around */
static void choose_next(gtg_app_handle_t app)
{
    if (gtg_dev_config_get_flags() & GTG_DEV_FLAG_VALID_CONFIG) {
        sync_cache(app);
        app->index = (app->index + 1) % (app->cache.len + 1);
    }

    show_chosen(app);
}

/* The start message, which takes the place of the end message: the next click wraps around to
 * the first departure */
static void show_start(gtg_app_handle_t app)
{
    stop_redraw(app);
    app->index = app->cache.len;
    gtg_lcd_print(&app->lcd, START_MSG_TOP, START_MSG_BOTTOM);
}

/* In the configuration mode with a valid configuration, tells the user how to stop it.
 * \return true when shown */
static bool show_config_hint(gtg_app_handle_t app)
{
    if (!gtg_https_server_is_running() || !(gtg_dev_config_get_flags() & GTG_DEV_FLAG_VALID_CONFIG)) {
        return false;
    }

    stop_redraw(app);
    gtg_lcd_print(&app->lcd, CONFIG_HINT_TOP, CONFIG_HINT_BOTTOM);
    return true;
}

static void toggle_config_mode(gtg_app_handle_t app)
{
    stop_redraw(app);

    if (gtg_https_server_is_running()) {
        config_mode_stop(app);
        show_start(app);
    } else {
        config_mode_start(app);
    }
}

/* The connection was lost while running, or came back. The station retries by itself, see
 * on_connection_retrying() and on_connection_failed() */
static void on_connection_changed(gtg_app_handle_t app, const gtg_event_t *event)
{
    if (!event->wifi.connected) {
        /* the device disconnected itself, e.g. to join another network from the configuration */
        if (event->wifi.requested) {
            return;
        }

        ESP_LOGW(TAG, "connection lost, the station retries");
        app->conn_lost = true;
        app->resume_redraw = app->redraw;
        stop_redraw(app);
        gtg_lcd_print(&app->lcd, CONN_LOST_TOP, "Retrying...");
        return;
    }

    if (!app->conn_lost) {
        return;
    }

    ESP_LOGI(TAG, "connection back");
    app->conn_lost = false;

    /* the cache is unchanged meanwhile, it only changes in sync_cache() */
    if (app->resume_redraw && app->index < app->cache.len) {
        redraw_departure(app);
    } else if (!show_config_hint(app)) {
        show_start(app);
    }
}

static void on_connection_retrying(gtg_app_handle_t app, const gtg_event_t *event)
{
    if (!app->conn_lost) {
        return;
    }

    /* room for any uint8_t, the counts are single digits and fit the screen */
    char bottom[sizeof("Retrying: 255/255")];
    snprintf(bottom, sizeof(bottom), "Retrying: %u/%u", event->wifi.retry, event->wifi.max_retries);
    gtg_lcd_print(&app->lcd, CONN_LOST_TOP, bottom);
}

/* The station gave up: on the lost connection, which the user reconnects in the configuration,
 * or on a network the user chose in the configuration, see on_config_connection_failed() */
static void on_connection_failed(gtg_app_handle_t app)
{
    if (!app->conn_lost) {
        on_config_connection_failed(app);
        return;
    }

    ESP_LOGW(TAG, "could not reconnect, starting the configuration mode");
    app->conn_lost = false;
    app->resume_redraw = false;
    enter_config_mode(app, CONN_LOST_TOP, "Starting config");
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

    /* every departure is fetched once, one request per stop: a fetch fills every node on its
     * stop, so the later ones on a fetched stop are fresh and make no request */
    gtg_lcd_print(&app->lcd, "Fetching...", "");
    sync_cache(app);

    /* before the first departure, so the first click shows it */
    app->index = app->cache.len;

    bool config_started = false;
    for (size_t i = 0; i < app->cache.len && !config_started; i++) {
        config_started = !refresh_departure(app, i);
    }

    /* after the first setup the configuration mode is still on */
    if (!config_started && !show_config_hint(app)) {
        show_start(app);
    }

    gtg_event_t event;

    while (true) {
        /* the shown departure is redrawn when its times change, which isn't an event, so
         * watch_task() still puts the device to sleep after 5 minutes without one */
        TickType_t wait = portMAX_DELAY;
        if (app->redraw) {
            TickType_t now = xTaskGetTickCount();
            /* wraps around like the tick count */
            wait = (TickType_t)(app->redraw_at - now) <= (TickType_t)(portMAX_DELAY / 2)
                 ? app->redraw_at - now : 0;
        }

        if (gtg_events_receive(&event, wait) != ESP_OK) {
            if (app->redraw) {
                on_redraw_timer(app);
            }
            continue;
        }

        /* the clicks would change the screen, which shows the login password or the configuration */
        if (gtg_https_server_is_running() && !is_config_button(event.type)) {
            ESP_LOGI(TAG, "event: %s, ignored during the configuration", gtg_event_type_name(event.type));
            continue;
        }

        {
            switch(event.type) {
                case GTG_EV_BUTTON_CLICKED:
                    choose_next(app);
                    break;

                case GTG_EV_BUTTON_TRIPLE_CLICKED:
                    log_event(&event);
                    go_to_sleep(app, "triple-click");
                    continue;

                case GTG_EV_BUTTON_SHORT_HOLD:
                    toggle_config_mode(app);
                    break;

                case GTG_EV_BUTTON_LONG_HOLD:
                    log_event(&event);
                    factory_reset(app);
                    continue;

                case GTG_EV_CONNECTION_CHANGED:
                    on_connection_changed(app, &event);
                    break;

                case GTG_EV_CONNECTION_RETRYING:
                    on_connection_retrying(app, &event);
                    break;

                case GTG_EV_CONNECTION_FAILED:
                    on_connection_failed(app);
                    break;

                case GTG_EV_API_KEY_REJECTED:
                    // print error message and wait for configuration mode
                    break;

                case GTG_EV_CONFIG_APPLIED:
                case GTG_EV_TARGET_ADDED:
                case GTG_EV_TARGET_REMOVED:
                    /* only while the configuration is valid, so only with departures left */
                    show_config_hint(app);
                    break;

                case GTG_EV_CONFIG_LOGIN:
                    stop_redraw(app);
                    show_config_logged_in(app);
                    break;

                default:
                    break;
            }

            log_event(&event);
        }
    }
}
