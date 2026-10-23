#ifndef GTG_WIFI_STATION_H_
#define GTG_WIFI_STATION_H_

#include "esp_err.h"

/**
 * \brief Connects to a WiFi network as a station and blocks until connected or failed
 *
 * Only WPA2-PSK or stronger networks are accepted (WPA3-SAE is preferred when
 * the AP supports it) and Protected Management Frames are used when available.
 * Open, WEP and WPA networks are rejected.
 *
 * esp_netif_init() and esp_event_loop_create_default() must have been
 * called once beforehand.
 *
 * On failure all resources are released again, so wifi_station_deinit()
 * must not be called.
 *
 * \param *ssid SSID of the network, 1-32 characters
 * \param *pass password of the network, 8-63 characters
 *
 * \return ESP_OK                connected and got an IP address
 * \return ESP_ERR_INVALID_ARG   ssid or pass is NULL or has an invalid length
 * \return ESP_ERR_INVALID_STATE station is already initialized
 * \return ESP_FAIL              could not connect after the maximum number of retries
 * \return other                 error from the underlying esp_wifi / esp_netif calls
 */
esp_err_t wifi_station_init(const char *ssid, const char *pass);

/**
 * \brief Disconnects from the network and releases the resources taken by wifi_station_init()
 */
void wifi_station_deinit(void);

#endif
