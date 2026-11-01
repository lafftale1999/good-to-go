#ifndef GTG_WIFI_STATION_H_
#define GTG_WIFI_STATION_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

typedef enum {
    WIFI_STATION_IDLE,          /*!< no connection has been requested */
    WIFI_STATION_CONNECTING,    /*!< connecting or retrying */
    WIFI_STATION_CONNECTED,     /*!< connected and got an IP address */
    WIFI_STATION_FAILED,        /*!< gave up after the maximum number of retries */
} wifi_station_state_t;

typedef struct {
    char ssid[33];              /*!< NUL terminated SSID */
    int8_t rssi;                /*!< signal strength in dBm, higher is stronger */
    bool password_protected;    /*!< false for open and OWE networks */
} wifi_station_scan_result_t;

/**
 * \brief Enables the station interface without connecting to a network
 *
 * Can run at the same time as the softAP, see gtg_wifi_enable().
 *
 * esp_netif_init() and esp_event_loop_create_default() must have been
 * called once beforehand.
 *
 * On failure all resources are released again, so wifi_station_deinit()
 * must not be called.
 *
 * \return ESP_OK                station enabled
 * \return ESP_ERR_INVALID_STATE station is already initialized
 * \return other                 error from the underlying esp_wifi / esp_netif calls
 */
esp_err_t wifi_station_init(void);

/**
 * \brief Starts connecting to a WiFi network without waiting for the result
 *
 * Only WPA2-PSK or stronger networks are accepted (WPA3-SAE is preferred when
 * the AP supports it) and Protected Management Frames are used when available.
 * Open, WEP and WPA networks are rejected.
 *
 * Follow the progress with wifi_station_get_state() or wifi_station_wait_connected().
 * A network that is already connected is disconnected first.
 *
 * \param *ssid SSID of the network, 1-32 characters
 * \param *pass password of the network, 8-63 characters
 *
 * \return ESP_OK                connecting
 * \return ESP_ERR_INVALID_ARG   ssid or pass is NULL or has an invalid length
 * \return ESP_ERR_INVALID_STATE station is not initialized or is already connecting
 * \return other                 error from the underlying esp_wifi calls
 */
esp_err_t wifi_station_connect(const char *ssid, const char *pass);

/**
 * \brief Scans for nearby networks and blocks until the scan is done
 *
 * Takes around 2 seconds. A running softAP has to leave its channel during the
 * scan, so its clients see a short pause in traffic.
 *
 * Hidden networks are left out and a network seen from several access points
 * is only listed once, with its strongest signal.
 *
 * \param *results     filled with the strongest networks, strongest first
 * \param max_results  number of entries results can hold
 * \param *count       set to the number of entries written
 *
 * \return ESP_OK                scan done, count can be 0
 * \return ESP_ERR_INVALID_ARG   results or count is NULL
 * \return ESP_ERR_INVALID_STATE station is not initialized or is connecting
 * \return other                 error from the underlying esp_wifi calls
 */
esp_err_t wifi_station_scan(wifi_station_scan_result_t *results, size_t max_results, size_t *count);

/**
 * \brief Returns the connection state
 *
 * \param *reason optional, set to the wifi_err_reason_t of the last disconnect,
 *                0 when connected or no disconnect has happened
 */
wifi_station_state_t wifi_station_get_state(uint8_t *reason);

/**
 * \brief Copies the SSID of the network the station is connected to
 *
 * \param *ssid      receives the NUL terminated SSID
 * \param ssid_size  at least 33, an SSID is up to 32 characters
 *
 * \return ESP_OK                copied
 * \return ESP_ERR_INVALID_ARG   ssid is NULL or too small
 * \return ESP_ERR_INVALID_STATE not connected
 */
esp_err_t wifi_station_get_ssid(char *ssid, size_t ssid_size);

/**
 * \brief Blocks until the station is connected and has an IP address
 *
 * Failed attempts don't end the wait, a new wifi_station_connect() can still succeed.
 *
 * \param ticks maximum time to wait, portMAX_DELAY waits forever
 *
 * \return ESP_OK                connected
 * \return ESP_ERR_TIMEOUT       not connected within the given time
 * \return ESP_ERR_INVALID_STATE station is not initialized
 */
esp_err_t wifi_station_wait_connected(TickType_t ticks);

/**
 * \brief Blocks until the connection started by wifi_station_connect() succeeds or gives up
 *
 * \param ticks maximum time to wait, portMAX_DELAY waits forever
 *
 * \return ESP_OK                connected
 * \return ESP_FAIL              gave up after the retries, see wifi_station_get_state() for the reason
 * \return ESP_ERR_TIMEOUT       neither within the given time
 * \return ESP_ERR_INVALID_STATE station is not initialized
 */
esp_err_t wifi_station_wait_result(TickType_t ticks);

/**
 * \brief Disconnects from the network and releases the resources taken by wifi_station_init()
 *
 * A running softAP is not affected.
 */
void wifi_station_deinit(void);

#endif
