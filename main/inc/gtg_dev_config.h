#ifndef GTG_DEV_CONFIG_H_
#define GTG_DEV_CONFIG_H_

#include <stddef.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#include "gtg_trafiklab_ds.h"
#include "gtg_api_keys.h"

#define GTG_DEV_CONFIG_SSID_MAX_LEN 32
#define GTG_DEV_CONFIG_PASS_MAX_LEN 63
#define GTG_DEV_CONFIG_MAX_TARGETS  8

/**
 * The runtime configuration of the device. It is only kept in RAM for now, a later version
 * saves it to NVS. The single instance is owned by this module and reached through the
 * functions below, which copy values in and out under a lock, as it is used from several tasks.
 */
typedef struct _gtg_dev_config_ {
    char                    wifi_ssid[GTG_DEV_CONFIG_SSID_MAX_LEN + 1];
    char                    wifi_pass[GTG_DEV_CONFIG_PASS_MAX_LEN + 1];

    /* local time of the last change, 0 when it happened before the clock was set by NTP */
    time_t                  last_changed;

    /* room for GTG_DEV_CONFIG_MAX_TARGETS is allocated once, len of them are in use */
    trafiklab_target_list_t realtime_targets;

    char                    trafiklab_api_key[GTG_API_KEY_MAX_LEN + 1];
    char                    google_maps_api_key[GTG_API_KEY_MAX_LEN + 1];
} gtg_dev_config_t;

/**
 * \brief Prepares the empty configuration, must be called once before the other functions
 *
 * \return ESP_OK                ready
 * \return ESP_ERR_INVALID_STATE already initialized
 * \return ESP_ERR_NO_MEM        out of memory
 */
esp_err_t gtg_dev_config_init(void);

/**
 * \brief Stores the credentials of the WiFi network the station connected to
 *
 * last_changed is only updated when they differ from the stored ones.
 *
 * \param *ssid  1-GTG_DEV_CONFIG_SSID_MAX_LEN characters
 * \param *pass  0-GTG_DEV_CONFIG_PASS_MAX_LEN characters
 *
 * \return ESP_OK                stored
 * \return ESP_ERR_INVALID_ARG   NULL, empty or too long
 * \return ESP_ERR_INVALID_STATE not initialized
 */
esp_err_t gtg_dev_config_set_wifi(const char *ssid, const char *pass);

/**
 * \brief Stores a verified API key
 *
 * last_changed is only updated when it differs from the stored one.
 *
 * \param which the service the key belongs to
 * \param *key  1-GTG_API_KEY_MAX_LEN characters
 *
 * \return ESP_OK                stored
 * \return ESP_ERR_INVALID_ARG   unknown service, NULL, empty or too long
 * \return ESP_ERR_INVALID_STATE not initialized
 */
esp_err_t gtg_dev_config_set_api_key(gtg_api_key_t which, const char *key);

/**
 * \brief Copies a stored API key
 *
 * \param which     the service the key belongs to
 * \param *out      receives the NUL terminated key
 * \param out_size  size of out, GTG_API_KEY_MAX_LEN + 1 always fits
 *
 * \return ESP_OK                key copied
 * \return ESP_ERR_INVALID_ARG   unknown service or out is NULL
 * \return ESP_ERR_NOT_FOUND     no key stored for the service
 * \return ESP_ERR_INVALID_SIZE  out is too small
 * \return ESP_ERR_INVALID_STATE not initialized
 */
esp_err_t gtg_dev_config_get_api_key(gtg_api_key_t which, char *out, size_t out_size);

/**
 * \brief Adds a departure to track to realtime_targets
 *
 * A target is unique by its stop id and its departure's line, mode and destination.
 * The stop's name and town are only kept for showing it.
 *
 * \param *target   the stop and departure, with NUL terminated fields and a non-empty
 *                  stop id, mode and destination
 * \param *count    receives the number of targets after adding, can be NULL
 *
 * \return ESP_OK                added
 * \return ESP_ERR_INVALID_ARG   NULL or an empty or unterminated field
 * \return ESP_ERR_NOT_ALLOWED   the departure is already tracked
 * \return ESP_ERR_INVALID_SIZE  GTG_DEV_CONFIG_MAX_TARGETS are already tracked
 * \return ESP_ERR_INVALID_STATE not initialized
 */
esp_err_t gtg_dev_config_add_target(const trafiklab_target_t *target, size_t *count);

/**
 * \brief Returns whether the departure from the stop is in realtime_targets
 */
bool gtg_dev_config_has_target(const char *stop_id, const gtg_departure_option_t *departure);

#endif
