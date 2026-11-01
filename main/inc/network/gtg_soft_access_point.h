#ifndef GTG_SOFT_AP_H_
#define GTG_SOFT_AP_H_

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

/**
 * \brief Struct for accessing the wifi credentials
 */
typedef struct wifi_ap_meta_ {
    char ssid[9];
    char pass[9];
} wifi_ap_meta_t;


/**
 * \brief Launches the WiFi AP
 *
 * Can run at the same time as the station, see gtg_wifi_enable(). While the
 * station is connected the AP follows the channel of the station's network.
 *
 * esp_netif_init() and esp_event_loop_create_default() must have been
 * called once beforehand.
 *
 * \param *wifi_ap_meta structure with credentials
 */
void wifi_softap_init(wifi_ap_meta_t *wifi_ap_meta);

/**
 * \brief Stops the WiFi AP and releases the resources taken by wifi_softap_init()
 *
 * A running station is not affected.
 */
void wifi_softap_deinit(void);

/**
 * \brief Whether the AP is started, between wifi_softap_init() and wifi_softap_deinit()
 */
bool wifi_softap_is_running(void);

/**
 * \brief Whether a client is connected to the AP, false when it isn't running
 *
 * Doesn't block, so unlike wifi_softap_wait_for_client() it is safe to call while another task
 * may stop the AP.
 */
bool wifi_softap_has_client(void);

/**
 * \brief Blocks until a client is connected to the AP
 *
 * Returns immediately if a client is already connected.
 *
 * \param ticks maximum time to wait, portMAX_DELAY waits forever
 *
 * \return ESP_OK                a client is connected
 * \return ESP_ERR_TIMEOUT       no client connected within the given time
 * \return ESP_ERR_INVALID_STATE the AP is not initialized
 */
esp_err_t wifi_softap_wait_for_client(TickType_t ticks);

#endif
