#ifndef GTG_MDNS_H_
#define GTG_MDNS_H_

#include "esp_err.h"

/**
 * \brief Starts mDNS so the device responds to goodtogo.local
 *
 * Also advertises the HTTPS server as an _https._tcp service on port 443.
 * mDNS follows the default WiFi interfaces automatically, so it works both
 * in softAP and station mode and when switching between them.
 *
 * esp_netif_init() and esp_event_loop_create_default() must have been
 * called once beforehand.
 *
 * \return ESP_OK                mDNS started
 * \return ESP_ERR_INVALID_STATE mDNS is already running
 * \return other                 error from the mdns component
 */
esp_err_t gtg_mdns_init(void);

/**
 * \brief Stops mDNS and releases the resources taken by gtg_mdns_init()
 */
void gtg_mdns_deinit(void);

#endif
