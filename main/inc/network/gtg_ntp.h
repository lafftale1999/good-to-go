#ifndef GTG_NTP_H_
#define GTG_NTP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

/**
 * \brief Starts SNTP and sets the local timezone to Swedish time (CET/CEST)
 *
 * Once started, SNTP keeps the system clock updated in the background every
 * CONFIG_LWIP_SNTP_UPDATE_DELAY ms. Daylight saving time is handled
 * automatically through the timezone rules.
 *
 * The device must be connected to a network, e.g. through wifi_station_init().
 *
 * \return ESP_OK                SNTP started
 * \return ESP_ERR_INVALID_STATE SNTP is already initialized
 * \return other                 error from esp_netif_sntp_init()
 */
esp_err_t gtg_ntp_init(void);

/**
 * \brief Blocks until the system time has been synchronized at least once
 *
 * \param timeout_ms maximum time to wait
 *
 * \return ESP_OK                time is synchronized
 * \return ESP_ERR_TIMEOUT       no sync within timeout_ms
 * \return ESP_ERR_INVALID_STATE gtg_ntp_init() has not been called
 */
esp_err_t gtg_ntp_wait_for_sync(uint32_t timeout_ms);

/**
 * \brief Uses the clock kept through deep sleep, before SNTP has synced it
 *
 * The RTC keeps the system time through deep sleep. This sets the timezone and
 * GTG_DEV_FLAG_TIME_SYNCED, so the time can be used at once. The sleep clock drifts, so start
 * SNTP as well, which corrects it in the background. Its first sync posts GTG_EV_TIME_CORRECTED
 * with how far it moved the clock, when it moved it a second or more.
 * gtg_ntp_wait_for_sync() still waits for SNTP.
 *
 * \return ESP_OK                the kept clock is in use
 * \return ESP_ERR_INVALID_STATE the clock was never set, e.g. after a power on
 */
esp_err_t gtg_ntp_restore_time(void);

/**
 * \brief Whether SNTP has synced the clock this boot, unlike gtg_ntp_is_time_set() not the
 *        clock kept through deep sleep
 */
bool gtg_ntp_is_synced(void);

/**
 * \brief Checks whether the system time can be used
 *
 * \return true once SNTP has synced it, or the clock kept through deep sleep is in use, see
 *         gtg_ntp_restore_time()
 */
bool gtg_ntp_is_time_set(void);

/**
 * \brief Gets the current Swedish local time
 *
 * \param *timeinfo filled with the local time
 *
 * \return ESP_OK                timeinfo filled
 * \return ESP_ERR_INVALID_ARG   timeinfo is NULL
 * \return ESP_ERR_INVALID_STATE time has not been synchronized yet
 */
esp_err_t gtg_ntp_get_local_time(struct tm *timeinfo);

/**
 * \brief Formats the current Swedish local time as "YYYY-MM-DD HH:MM:SS"
 *
 * \param *buf  output buffer
 * \param len   size of buf, at least 20 bytes
 *
 * \return ESP_OK                buf filled
 * \return ESP_ERR_INVALID_ARG   buf is NULL or too small
 * \return ESP_ERR_INVALID_STATE time has not been synchronized yet
 */
esp_err_t gtg_ntp_get_local_time_str(char *buf, size_t len);

/**
 * \brief Stops SNTP. The system clock keeps running but is no longer corrected
 */
void gtg_ntp_deinit(void);

#endif
