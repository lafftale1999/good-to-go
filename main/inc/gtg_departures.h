#ifndef GTG_DEPARTURES_H_
#define GTG_DEPARTURES_H_

#include <stddef.h>
#include "esp_err.h"

#define GTG_DEPARTURES_MAX_OPTIONS      32
#define GTG_DEPARTURE_LINE_MAX_LEN      31  /*!< route designation, or the route name for named lines */
#define GTG_DEPARTURE_MODE_MAX_LEN      15  /*!< BUS, METRO, TRAIN, TRAM, TAXI or BOAT */
#define GTG_DEPARTURE_DEST_MAX_LEN      63

/** A line and destination that departs from a stop, one per unique combination */
typedef struct {
    char line[GTG_DEPARTURE_LINE_MAX_LEN + 1];
    char mode[GTG_DEPARTURE_MODE_MAX_LEN + 1];
    char destination[GTG_DEPARTURE_DEST_MAX_LEN + 1];
} gtg_departure_option_t;

/**
 * \brief Lists the unique departures from a stop group in the next 60 minutes
 *
 * A departure is unique by its route designation, transport mode and destination name.
 * Canceled departures are counted, as the line still serves the stop. The options are sorted
 * by line, numerically where the line starts with a number, then mode and destination.
 *
 * The response is parsed while it is received, one departure at a time, so a busy stop with
 * a large response needs no more memory than a quiet one. Blocks for the length of the
 * request. The Trafiklab key must be verified, see gtg_api_keys.h.
 *
 * \param *stop_id  stop group id, 1-9 digits
 * \param *options  receives the options, room for GTG_DEPARTURES_MAX_OPTIONS
 * \param *count    receives the number of options, more than GTG_DEPARTURES_MAX_OPTIONS are dropped
 *
 * \return ESP_OK                   options listed, *count can be 0
 * \return ESP_ERR_INVALID_ARG      invalid stop id or NULL argument
 * \return ESP_ERR_INVALID_STATE    the Trafiklab key is not verified
 * \return ESP_ERR_NOT_FOUND        Trafiklab doesn't know the stop
 * \return ESP_ERR_NOT_ALLOWED      Trafiklab rejected the key
 * \return ESP_ERR_INVALID_RESPONSE Trafiklab answered with an unexpected response
 * \return ESP_ERR_NO_MEM           out of memory
 * \return other                    the request failed, e.g. no internet access
 */
esp_err_t gtg_departures_list_options(const char *stop_id, gtg_departure_option_t *options, size_t *count);

#endif
