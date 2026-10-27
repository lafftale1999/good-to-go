#ifndef GTG_HTTPS_SERVER_H_
#define GTG_HTTPS_SERVER_H_

#include "esp_err.h"

/**
 * \brief Starts the HTTPS server on port 443
 *
 * The server listens on all interfaces, so it is reachable both in softAP
 * and station mode. It uses the certificate and private key embedded from
 * cred/servercert.pem and cred/prvtkey.pem.
 *
 * Static files are served from the "www" LittleFS partition, built from the
 * web/ directory. "/" serves index.html.
 *
 * WiFi configuration API, see gtg_wifi_station.h:
 *  - POST /api/wifi/connect {"ssid": "...", "pass": "..."} starts connecting the station
 *  - GET  /api/wifi/status  returns {"state": "idle|connecting|connected|failed", "reason": n}
 *
 * The network interface must be up, e.g. through wifi_softap_init() or
 * wifi_station_init().
 *
 * \return ESP_OK                server started
 * \return ESP_ERR_INVALID_STATE server is already running
 * \return other                 error from mounting the www partition or httpd_ssl_start()
 */
esp_err_t gtg_https_server_init(void);

/**
 * \brief Stops the HTTPS server and releases the resources taken by gtg_https_server_init()
 */
void gtg_https_server_deinit(void);

#endif
