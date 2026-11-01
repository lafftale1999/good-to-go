#ifndef GTG_STOP_SEARCH_H_
#define GTG_STOP_SEARCH_H_

#include <stddef.h>
#include "esp_err.h"

#define GTG_STOP_SEARCH_MAX_RESULTS 10
#define GTG_STOP_QUERY_MAX_LEN      128     /*!< bytes, UTF-8 */
#define GTG_STOP_ID_MAX_LEN         9       /*!< Trafiklab stop group id, e.g. "740046131" */
#define GTG_STOP_NAME_MAX_LEN       63
#define GTG_STOP_TOWN_MAX_LEN       47

typedef struct {
    char id[GTG_STOP_ID_MAX_LEN + 1];       /*!< stop group id */
    char name[GTG_STOP_NAME_MAX_LEN + 1];   /*!< stop group name */
    char town[GTG_STOP_TOWN_MAX_LEN + 1];   /*!< postal town, empty when it could not be resolved */
} gtg_stop_t;

/**
 * \brief Searches Trafiklab for stop groups by name and resolves the postal town of each
 *
 * Stop groups without transport modes or without daily stop times are skipped. The rest are
 * sorted by average daily stop times, busiest first, and the top GTG_STOP_SEARCH_MAX_RESULTS
 * are kept. Their postal towns are resolved through Google reverse geocoding over a single
 * HTTPS connection. A stop whose town can't be resolved is still returned, with an empty town.
 *
 * Makes up to GTG_STOP_SEARCH_MAX_RESULTS + 1 requests and blocks until they are done, which
 * takes a few seconds. Both API keys must be verified, see gtg_api_keys.h.
 *
 * \param *query    stop name, 1-GTG_STOP_QUERY_MAX_LEN bytes without control characters
 * \param *results  receives the stops, room for GTG_STOP_SEARCH_MAX_RESULTS
 * \param *count    receives the number of stops in results
 *
 * \return ESP_OK                   search done, *count can be 0
 * \return ESP_ERR_INVALID_ARG      invalid query or NULL argument
 * \return ESP_ERR_INVALID_STATE    the API keys are not verified
 * \return ESP_ERR_NOT_ALLOWED      Trafiklab rejected the key
 * \return ESP_ERR_INVALID_SIZE     Trafiklab's response is too large, the query matches too many stops
 * \return ESP_ERR_INVALID_RESPONSE Trafiklab answered with an unexpected response
 * \return ESP_ERR_NO_MEM           out of memory
 * \return other                    the request failed, e.g. no internet access
 */
esp_err_t gtg_stop_search(const char *query, gtg_stop_t *results, size_t *count);

#endif
