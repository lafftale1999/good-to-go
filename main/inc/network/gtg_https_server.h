#ifndef GTG_HTTPS_SERVER_H_
#define GTG_HTTPS_SERVER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
 * Login. Every start makes a new random password, see gtg_https_server_get_password(), which the
 * device shows on its LCD. Every API endpoint but the login answers 401 Unauthorized without a valid
 * session, the static files are open, as the page shows the login itself:
 *  - POST /api/login {"password": "..."} sets the session cookie, an HttpOnly cookie the browser
 *    sends with every request. A session ends 5 minutes after its last successful request, one
 *    answered with a 2xx status, and when the server stops. Answers 401 for a wrong password, and
 *    429 for a while after 5 wrong in a row
 *  - Only successful requests with a valid session, and logins, count as activity, see
 *    gtg_https_server_last_activity_us()
 *
 * Device state, see gtg_dev_flag_t in gtg_dev_config.h. The page starts at the first step that isn't done:
 *  - GET  /api/status returns {"connected": bool, "time_synced": bool, "keys_verified": bool,
 *                              "has_departures": bool, "valid_config": bool}
 *  - POST /api/config/apply   the "Done" button, returns like /api/status, with 409 Conflict while
 *                             the configuration isn't valid yet. It changes nothing, the
 *                             configuration becomes valid by itself
 *
 * WiFi configuration API, see gtg_wifi_station.h:
 *  - POST /api/wifi/connect {"ssid": "...", "pass": "..."} starts connecting the station
 *  - GET  /api/wifi/status  returns {"state": "idle|connecting|connected|failed", "reason": n}
 *  - GET  /api/wifi/scan    returns [{"ssid": "...", "rssi": n, "secure": bool}, ...], strongest first
 *
 * API key configuration, see gtg_api_keys.h. Keys can only be added once the station is connected:
 *  - POST /api/keys/trafiklab {"key": "..."} verifies and stores the Trafiklab key
 *  - POST /api/keys/google    {"key": "..."} verifies and stores the Google key
 *  - GET  /api/keys/status    returns {"trafiklab": bool, "google": bool}
 *
 * Stop configuration, see gtg_stop_search.h. Needs the station connected and both keys verified:
 *  - POST /api/stops/search {"query": "..."} returns [{"id": "...", "name": "...", "town": "..."}, ...]
 *
 * Departure configuration, see gtg_departures.h. Needs the station connected, the Trafiklab key verified
 * and the clock set:
 *  - POST /api/departures/available {"stop_id": "..."} returns
 *    {"options": [{"line": "...", "mode": "...", "destination": "...", "tracked": bool}, ...], "samples": n, "failed": n}
 *
 * Tracked departures, see gtg_dev_config.h:
 *  - GET  /api/targets returns {"targets": [{"stop": {"id", "name", "town"},
 *                                            "departure": {"line", "mode", "destination"}}, ...], "max": n}
 *  - POST /api/targets {"stop": {...}, "departure": {...}} adds the departure to realtime_targets
 *    and returns like GET
 *  - DELETE /api/targets {"stop_id": "...", "departure": {...}} removes the departure from
 *    realtime_targets and returns like GET
 *
 * Factory reset, see gtg_dev_config_factory_reset():
 *  - POST /api/factory-reset {"confirm": "factory-reset"} erases the stored configuration and
 *    restarts the device a second after responding
 *
 * gtg_api_keys_init() must have been called before keys are added.
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

/**
 * \brief Whether the server is running
 */
bool gtg_https_server_is_running(void);

/**
 * \brief esp_timer_get_time() of the last successful logged in request or login, or of the start
 *        while there has been none
 *
 * The page's own polling counts too, so an open page keeps the server in use. Requests without a
 * valid session, or answered with an error, don't, so they can't keep the device awake.
 */
int64_t gtg_https_server_last_activity_us(void);

/* letters and digits that can't be mistaken for each other, and "PASS: " with it fills an LCD row */
#define GTG_HTTPS_PASSWORD_LEN  10

/**
 * \brief Copies the login password of the running server, made anew at every start
 *
 * \param *out      receives the password, NUL terminated
 * \param out_size  at least GTG_HTTPS_PASSWORD_LEN + 1
 *
 * \return ESP_OK                copied
 * \return ESP_ERR_INVALID_ARG   out is NULL or too small
 * \return ESP_ERR_INVALID_STATE the server isn't running
 */
esp_err_t gtg_https_server_get_password(char *out, size_t out_size);

#endif
