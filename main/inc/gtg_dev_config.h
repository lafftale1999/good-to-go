#ifndef GTG_DEV_CONFIG_H_
#define GTG_DEV_CONFIG_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#include "gtg_trafiklab_ds.h"
#include "gtg_api_keys.h"

#define GTG_DEV_CONFIG_SSID_MAX_LEN 32
#define GTG_DEV_CONFIG_PASS_MAX_LEN 63
#define GTG_DEV_CONFIG_MAX_TARGETS  8

/**
 * Runtime state of the device, one bit each in gtg_dev_config_t.flags. They are not saved,
 * the modules that own each state keep them up to date while the device runs
 */
typedef enum {
    GTG_DEV_FLAG_CONNECTED      = 1 << 0,   /*!< the station is connected with an IP address */
    GTG_DEV_FLAG_TIME_SYNCED    = 1 << 1,   /*!< the clock has been set by NTP */
    GTG_DEV_FLAG_KEYS_VERIFIED  = 1 << 2,   /*!< every API key has been verified this boot */
    GTG_DEV_FLAG_HAS_DEPARTURES = 1 << 3,   /*!< realtime_targets holds at least one target */
    GTG_DEV_FLAG_VALID_CONFIG   = 1 << 4,   /*!< connected, keys verified and has departures, follows them by itself */
} gtg_dev_flag_t;

/**
 * The runtime configuration of the device. The single instance is owned by this module and
 * reached through the functions below, which copy values in and out under a lock, as it is
 * used from several tasks.
 *
 * Every function that changes the configuration also saves it to NVS, in plain text. A failed
 * save is logged and the change is kept in RAM. gtg_dev_config_load() reads it back.
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

    /* gtg_dev_flag_t bits, runtime only */
    uint32_t                flags;
} gtg_dev_config_t;

/**
 * \brief Prepares the empty configuration, must be called once before the other functions
 *
 * NVS must have been initialized with nvs_flash_init(). The stored configuration is not read,
 * see gtg_dev_config_load().
 *
 * \return ESP_OK                ready
 * \return ESP_ERR_INVALID_STATE already initialized
 * \return ESP_ERR_NO_MEM        out of memory
 */
esp_err_t gtg_dev_config_init(void);

/**
 * \brief Replaces the runtime configuration with the one stored in NVS
 *
 * The stored configuration is checked first. When it is missing or invalid, the runtime
 * configuration is left as it is.
 *
 * \return ESP_OK                   loaded
 * \return ESP_ERR_NOT_FOUND        nothing is stored, e.g. on first boot or after a factory reset
 * \return ESP_ERR_INVALID_VERSION  stored by firmware with another format
 * \return ESP_ERR_INVALID_SIZE     the stored targets don't fit
 * \return ESP_ERR_INVALID_STATE    not initialized, or a stored target is invalid
 * \return ESP_ERR_NO_MEM           out of memory
 * \return other                    error from NVS
 */
esp_err_t gtg_dev_config_load(void);

/**
 * \brief Writes the runtime configuration to NVS
 *
 * Changes are saved automatically, this is for saving without a change.
 *
 * \return ESP_OK                saved
 * \return ESP_ERR_INVALID_STATE not initialized
 * \return other                 error from NVS, e.g. ESP_ERR_NVS_NOT_ENOUGH_SPACE
 */
esp_err_t gtg_dev_config_save(void);

/**
 * \brief Erases the configuration stored in NVS and clears the runtime configuration
 *
 * The runtime configuration is cleared also when erasing fails. Other modules keep their own
 * state, e.g. the WiFi connection and the verified keys, so restart the device afterwards.
 *
 * \return ESP_OK                erased
 * \return ESP_ERR_INVALID_STATE not initialized
 * \return other                 error from NVS
 */
esp_err_t gtg_dev_config_factory_reset(void);

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
 * \brief Copies the stored WiFi credentials
 *
 * \param *ssid       receives the SSID, room for GTG_DEV_CONFIG_SSID_MAX_LEN + 1
 * \param *pass       receives the password, room for GTG_DEV_CONFIG_PASS_MAX_LEN + 1
 *
 * \return ESP_OK                copied
 * \return ESP_ERR_INVALID_ARG   NULL argument
 * \return ESP_ERR_NOT_FOUND     no credentials are stored
 * \return ESP_ERR_INVALID_STATE not initialized
 */
esp_err_t gtg_dev_config_get_wifi(char *ssid, char *pass);

/**
 * \brief Sets or clears a runtime flag, a change is logged
 *
 * GTG_DEV_FLAG_HAS_DEPARTURES follows realtime_targets by itself. GTG_DEV_FLAG_VALID_CONFIG
 * follows the other flags: it is set while the device is connected, the keys are verified and
 * it has departures, and cleared as soon as one of them is not. Setting it here is refused.
 * GTG_EV_CONFIG_APPLIED is posted each time it gets set.
 */
void gtg_dev_config_set_flag(gtg_dev_flag_t flag, bool on);

/**
 * \brief Returns the gtg_dev_flag_t bits that are set
 */
uint32_t gtg_dev_config_get_flags(void);

/**
 * \brief Blocks until all the given gtg_dev_flag_t bits are set
 *
 * \param flags  the bits to wait for
 * \param ticks  maximum time to wait, portMAX_DELAY waits forever
 *
 * \return ESP_OK                all the bits are set
 * \return ESP_ERR_TIMEOUT       not all set within the given time
 * \return ESP_ERR_INVALID_STATE not initialized
 */
esp_err_t gtg_dev_config_wait_flags(uint32_t flags, TickType_t ticks);

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
 * \brief Removes a departure from realtime_targets, the others keep their order
 *
 * \param *stop_id    stop group id of the target
 * \param *departure  line, mode and destination of the target
 *
 * \return ESP_OK                removed
 * \return ESP_ERR_INVALID_ARG   NULL argument
 * \return ESP_ERR_NOT_FOUND     the departure is not tracked
 * \return ESP_ERR_INVALID_STATE not initialized
 */
esp_err_t gtg_dev_config_remove_target(const char *stop_id, const gtg_departure_option_t *departure);

/**
 * \brief Copies realtime_targets in the order they were added
 *
 * \param *out      receives the targets, room for GTG_DEV_CONFIG_MAX_TARGETS always fits
 * \param max       number of targets out has room for
 * \param *count    receives the number of targets copied
 *
 * \return ESP_OK                targets copied, *count can be 0
 * \return ESP_ERR_INVALID_ARG   out or count is NULL
 * \return ESP_ERR_INVALID_SIZE  more targets than max, the first max are copied
 * \return ESP_ERR_INVALID_STATE not initialized
 */
esp_err_t gtg_dev_config_get_targets(trafiklab_target_t *out, size_t max, size_t *count);

/**
 * \brief A number that changes each time realtime_targets changes: added, removed, loaded or
 *        reset. Tells a copy of the targets that it is outdated
 */
uint32_t gtg_dev_config_get_targets_version(void);

/**
 * \brief Returns whether the departure from the stop is in realtime_targets
 */
bool gtg_dev_config_has_target(const char *stop_id, const gtg_departure_option_t *departure);

#endif
