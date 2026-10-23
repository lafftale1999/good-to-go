#ifndef GTG_SOFT_AP_H_
#define GTG_SOFT_AP_H_

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
 * esp_netif_init() and esp_event_loop_create_default() must have been
 * called once beforehand.
 *
 * \param *wifi_ap_meta structure with credentials
 */
void wifi_softap_init(wifi_ap_meta_t *wifi_ap_meta);

/**
 * \brief Stops the WiFi AP and releases the resources taken by wifi_softap_init()
 */
void wifi_softap_deinit(void);

#endif
