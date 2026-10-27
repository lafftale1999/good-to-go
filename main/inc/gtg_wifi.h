#ifndef GTG_WIFI_H_
#define GTG_WIFI_H_

#include "esp_err.h"
#include "esp_wifi.h"

/**
 * \brief Enables a WiFi interface without disturbing the other one
 *
 * The WiFi driver is shared between the softAP and the station. It is
 * initialized and started when the first interface is enabled, and the mode
 * is switched between AP, STA and APSTA as interfaces are enabled and disabled.
 *
 * The netif for the interface must be created before enabling it.
 *
 * Not thread safe, enable and disable interfaces from the same task.
 *
 * \param iface  WIFI_IF_AP or WIFI_IF_STA
 * \param config configuration applied to the interface before it is started
 *
 * \return ESP_OK              interface enabled
 * \return ESP_ERR_INVALID_ARG unsupported interface
 * \return other               error from the underlying esp_wifi calls
 */
esp_err_t gtg_wifi_enable(wifi_interface_t iface, const wifi_config_t *config);

/**
 * \brief Disables a WiFi interface without disturbing the other one
 *
 * The WiFi driver is stopped and deinitialized when no interface is left.
 *
 * \param iface WIFI_IF_AP or WIFI_IF_STA
 *
 * \return ESP_OK              interface disabled, or it was not enabled
 * \return ESP_ERR_INVALID_ARG unsupported interface
 * \return other               error from esp_wifi_set_mode()
 */
esp_err_t gtg_wifi_disable(wifi_interface_t iface);

#endif
