#ifndef GTG_API_KEYS_H_
#define GTG_API_KEYS_H_

#include <stddef.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#define GTG_API_KEY_MAX_LEN 64

typedef enum {
    GTG_API_KEY_TRAFIKLAB,      /*!< Trafiklab Realtime API */
    GTG_API_KEY_GOOGLE,         /*!< Google Geocoding API */
    GTG_API_KEY_COUNT,
} gtg_api_key_t;

/**
 * \brief Prepares the key verification, must be called once before the other functions
 *
 * Verified keys are stored in the device configuration, see gtg_dev_config.h, which must
 * have been initialized. Requests read them from there with gtg_dev_config_get_api_key().
 *
 * \return ESP_OK                ready
 * \return ESP_ERR_INVALID_STATE already initialized
 * \return ESP_ERR_NO_MEM        out of memory
 */
esp_err_t gtg_api_keys_init(void);

/**
 * \brief Verifies a key with a request to its service and stores it in the device configuration when it works
 *
 * Blocks for the length of the request, at most around 10 seconds. The station
 * must be connected to a network with internet access.
 *
 * \param which the service the key belongs to
 * \param *key  1-GTG_API_KEY_MAX_LEN letters, digits, '-' or '_'
 *
 * \return ESP_OK                   key works and is stored
 * \return ESP_ERR_INVALID_ARG      unknown service or invalid key format
 * \return ESP_ERR_NOT_ALLOWED      the service rejected the key
 * \return ESP_ERR_INVALID_RESPONSE the service answered with an unexpected response
 * \return ESP_ERR_INVALID_STATE    not initialized
 * \return other                    the request failed, e.g. no internet access
 */
esp_err_t gtg_api_keys_set(gtg_api_key_t which, const char *key);

/**
 * \brief Returns whether a verified key is stored for the service
 */
bool gtg_api_keys_is_set(gtg_api_key_t which);

/**
 * \brief Blocks until a verified key is stored for every service
 *
 * \param ticks maximum time to wait, portMAX_DELAY waits forever
 *
 * \return ESP_OK                all keys stored
 * \return ESP_ERR_TIMEOUT       not all keys stored within the given time
 * \return ESP_ERR_INVALID_STATE not initialized
 */
esp_err_t gtg_api_keys_wait_configured(TickType_t ticks);

#endif
