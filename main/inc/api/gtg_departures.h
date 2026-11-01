#ifndef GTG_DEPARTURES_H_
#define GTG_DEPARTURES_H_

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#define GTG_DEPARTURES_MAX_OPTIONS      64
#define GTG_DEPARTURES_SAMPLES          8   /*!< one hour each, spread over a Wednesday */
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
 * \brief Lists the unique departures from a stop group during a whole day
 *
 * Trafiklab returns the departures of one hour per request, so the next Wednesday (a week ahead
 * when today is Wednesday) is sampled GTG_DEPARTURES_SAMPLES times, from 00:00 every 2 h 59 min.
 * The samples are requested one after another over the same connection, and each is parsed while
 * it is received, one departure at a time, so a busy stop needs no more memory than a quiet one.
 *
 * A departure is unique by its route designation, transport mode and destination name. Canceled
 * departures are counted, as the line still serves the stop. The options are sorted by transport
 * mode, line (numerically where it starts with a number) and destination.
 *
 * A sample that fails is skipped and counted in *failed. Blocks for the length of the requests.
 * The Trafiklab key must be verified, see gtg_api_keys.h, and the clock set, see gtg_ntp.h.
 *
 * \param *stop_id  stop group id, 1-9 digits
 * \param *options  receives the options, room for GTG_DEPARTURES_MAX_OPTIONS
 * \param *count    receives the number of options, more than GTG_DEPARTURES_MAX_OPTIONS are dropped
 * \param *failed   receives the number of samples that failed, less than GTG_DEPARTURES_SAMPLES on ESP_OK
 *
 * \return ESP_OK                   at least one sample succeeded, *count can be 0
 * \return ESP_ERR_INVALID_ARG      invalid stop id or NULL argument
 * \return ESP_ERR_INVALID_STATE    the Trafiklab key is not verified
 * \return ESP_ERR_NOT_FINISHED     the clock has not been set by NTP yet
 * \return ESP_ERR_NOT_FOUND        Trafiklab doesn't know the stop
 * \return ESP_ERR_NOT_ALLOWED      Trafiklab rejected the key
 * \return ESP_ERR_INVALID_RESPONSE every sample failed with an unexpected response
 * \return ESP_ERR_NO_MEM           out of memory
 * \return other                    every sample failed, e.g. no internet access
 */
esp_err_t gtg_departures_list_options(const char *stop_id, gtg_departure_option_t *options,
                                      size_t *count, size_t *failed);

#define GTG_DEPARTURES_UPCOMING_MAX         3       /*!< soonest departures kept per target */
#define GTG_DEPARTURES_UPCOMING_WINDOW_S    3600    /*!< how far ahead departures are kept */

/** The soonest departures of one target */
typedef struct {
    uint16_t seconds[GTG_DEPARTURES_UPCOMING_MAX];  /*!< from the fetch time, soonest first */
    uint8_t  count;                                 /*!< 0-GTG_DEPARTURES_UPCOMING_MAX in use */
} gtg_departures_upcoming_t;

/**
 * \brief Fetches the next departures from a stop for several of its departures at once
 *
 * One request returns everything departing from the stop group from now on, which is matched
 * against every target by line, mode and destination like gtg_departures_list_options() tells
 * them apart. The realtime estimate is used when there is one, the timetable otherwise. Canceled
 * departures and those more than GTG_DEPARTURES_UPCOMING_WINDOW_S ahead are skipped, and each
 * target keeps its GTG_DEPARTURES_UPCOMING_MAX soonest. Blocks for the request.
 *
 * \param *stop_id     stop group id, 1-9 digits
 * \param *targets     the departures from the stop to look for
 * \param count        number of targets, at least 1
 * \param *upcoming    receives the departures of each target, room for count
 * \param *fetched_at  receives the time the seconds count from
 *
 * \return ESP_OK                   fetched, a target without departures has count 0
 * \return ESP_ERR_INVALID_ARG      invalid stop id, count 0 or NULL argument
 * \return ESP_ERR_INVALID_STATE    no Trafiklab key is stored
 * \return ESP_ERR_NOT_FINISHED     the clock has not been set by NTP yet
 * \return ESP_ERR_NOT_FOUND        Trafiklab doesn't know the stop
 * \return ESP_ERR_NOT_ALLOWED      Trafiklab rejected the key
 * \return ESP_ERR_INVALID_RESPONSE unexpected or cut response
 * \return ESP_ERR_NO_MEM           out of memory
 * \return other                    the request failed, e.g. no internet access
 */
esp_err_t gtg_departures_fetch_upcoming(const char *stop_id, const gtg_departure_option_t *targets,
                                        size_t count, gtg_departures_upcoming_t *upcoming,
                                        time_t *fetched_at);

#endif
