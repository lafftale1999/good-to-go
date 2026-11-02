#include "gtg_ntp.h"

#include <stdlib.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "esp_timer.h"

#include "gtg_dev_config.h"
#include "gtg_events.h"

/* POSIX TZ for Europe/Stockholm: CET (UTC+1), CEST (UTC+2) from the last
 * Sunday of March 02:00 to the last Sunday of October 03:00 */
#define GTG_NTP_TIMEZONE           "CET-1CEST,M3.5.0,M10.5.0/3"

#define GTG_NTP_SERVER_PRIMARY     "se.pool.ntp.org"
#define GTG_NTP_SERVER_SECONDARY   "pool.ntp.org"

#define GTG_NTP_TIME_STR_FORMAT    "%Y-%m-%d %H:%M:%S"
#define GTG_NTP_TIME_STR_MIN_LEN   20

/* a clock before this was never set, 2025-01-01 00:00 UTC */
#define GTG_NTP_MIN_VALID_TIME     1735689600

static const char *TAG = "ntp";

static bool s_initialized = false;
static volatile bool s_time_synced = false;

/* the clock kept through deep sleep, set before SNTP has synced it this boot */
static volatile bool s_time_restored = false;

/* the kept clock and esp_timer_get_time() when it was taken into use. esp_timer runs from the
 * crystal, so it tells what the kept clock should read at the sync, and how far SNTP moved it */
static int64_t s_restored_clock_us = 0;
static int64_t s_restored_timer_us = 0;

static int64_t clock_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* After the first sync on a wake, posts how far SNTP moved the kept clock */
static void post_correction(const struct timeval *tv)
{
    int64_t expected_us = s_restored_clock_us + (esp_timer_get_time() - s_restored_timer_us);
    int64_t synced_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
    int64_t correction_ms = (synced_us - expected_us) / 1000;

    /* rounded to whole seconds, which is what the departures count in */
    int32_t correction_s = (int32_t)((correction_ms + (correction_ms >= 0 ? 500 : -500)) / 1000);

    ESP_LOGI(TAG, "the clock kept through the sleep was %lld ms %s", (long long)llabs(correction_ms),
             correction_ms >= 0 ? "behind" : "ahead");

    if (correction_s == 0) {
        return;
    }

    gtg_event_t event = {
        .type = GTG_EV_TIME_CORRECTED,
        .time.correction_s = correction_s,
    };
    gtg_events_post(&event);
}

static void set_timezone(void)
{
    setenv("TZ", GTG_NTP_TIMEZONE, 1);
    tzset();
}

static void time_sync_notification_cb(struct timeval *tv)
{
    bool first_after_restore = s_time_restored && !s_time_synced;
    s_time_synced = true;

    if (first_after_restore) {
        post_correction(tv);
    }

    gtg_dev_config_set_flag(GTG_DEV_FLAG_TIME_SYNCED, true);

    char buf[GTG_NTP_TIME_STR_MIN_LEN];
    struct tm timeinfo;
    time_t now = tv->tv_sec;
    localtime_r(&now, &timeinfo);
    strftime(buf, sizeof(buf), GTG_NTP_TIME_STR_FORMAT, &timeinfo);

    ESP_LOGI(TAG, "time synchronized, local time: %s", buf);
}

esp_err_t gtg_ntp_init(void)
{
    if (s_initialized) {
        ESP_LOGE(TAG, "already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    set_timezone();

#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
                               ESP_SNTP_SERVER_LIST(GTG_NTP_SERVER_PRIMARY, GTG_NTP_SERVER_SECONDARY));
#else
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(GTG_NTP_SERVER_PRIMARY);
#endif
    config.sync_cb = time_sync_notification_cb;

    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&config), TAG, "esp_netif_sntp_init failed");

    s_initialized = true;

    ESP_LOGI(TAG, "SNTP started, server: %s, sync interval: %d ms",
             GTG_NTP_SERVER_PRIMARY, CONFIG_LWIP_SNTP_UPDATE_DELAY);

    return ESP_OK;
}

esp_err_t gtg_ntp_wait_for_sync(uint32_t timeout_ms)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_time_synced) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "time not synchronized within %lu ms", (unsigned long)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t gtg_ntp_restore_time(void)
{
    time_t now = time(NULL);
    if (now < GTG_NTP_MIN_VALID_TIME) {
        ESP_LOGW(TAG, "the clock wasn't kept through the sleep");
        return ESP_ERR_INVALID_STATE;
    }

    set_timezone();
    s_restored_clock_us = clock_us();
    s_restored_timer_us = esp_timer_get_time();
    s_time_restored = true;
    gtg_dev_config_set_flag(GTG_DEV_FLAG_TIME_SYNCED, true);

    char buf[GTG_NTP_TIME_STR_MIN_LEN];
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(buf, sizeof(buf), GTG_NTP_TIME_STR_FORMAT, &timeinfo);
    ESP_LOGI(TAG, "clock kept through the sleep, local time: %s", buf);

    return ESP_OK;
}

bool gtg_ntp_is_synced(void)
{
    return s_time_synced;
}

bool gtg_ntp_is_time_set(void)
{
    return s_time_synced || s_time_restored;
}

esp_err_t gtg_ntp_get_local_time(struct tm *timeinfo)
{
    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!gtg_ntp_is_time_set()) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t now;
    time(&now);
    localtime_r(&now, timeinfo);

    return ESP_OK;
}

esp_err_t gtg_ntp_get_local_time_str(char *buf, size_t len)
{
    if (buf == NULL || len < GTG_NTP_TIME_STR_MIN_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm timeinfo;
    ESP_RETURN_ON_ERROR(gtg_ntp_get_local_time(&timeinfo), TAG, "time not set");

    strftime(buf, len, GTG_NTP_TIME_STR_FORMAT, &timeinfo);

    return ESP_OK;
}

void gtg_ntp_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    esp_netif_sntp_deinit();
    s_initialized = false;
}
