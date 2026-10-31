#include "gtg_ntp.h"

#include <stdlib.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "gtg_dev_config.h"

/* POSIX TZ for Europe/Stockholm: CET (UTC+1), CEST (UTC+2) from the last
 * Sunday of March 02:00 to the last Sunday of October 03:00 */
#define GTG_NTP_TIMEZONE           "CET-1CEST,M3.5.0,M10.5.0/3"

#define GTG_NTP_SERVER_PRIMARY     "se.pool.ntp.org"
#define GTG_NTP_SERVER_SECONDARY   "pool.ntp.org"

#define GTG_NTP_TIME_STR_FORMAT    "%Y-%m-%d %H:%M:%S"
#define GTG_NTP_TIME_STR_MIN_LEN   20

static const char *TAG = "ntp";

static bool s_initialized = false;
static volatile bool s_time_synced = false;

static void time_sync_notification_cb(struct timeval *tv)
{
    s_time_synced = true;
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

    setenv("TZ", GTG_NTP_TIMEZONE, 1);
    tzset();

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

bool gtg_ntp_is_time_set(void)
{
    return s_time_synced;
}

esp_err_t gtg_ntp_get_local_time(struct tm *timeinfo)
{
    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_time_synced) {
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
