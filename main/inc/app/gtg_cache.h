#ifndef GTG_CACHE_H_
#define GTG_CACHE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#include "gtg_dev_config.h"
#include "gtg_departures.h"

#define GTG_CACHE_MAX_AGE_S     60      /*!< a node this old or older is fetched again */

/**
 * The coming departures of one realtime target. The seconds count from ts, so they are
 * read as estimates[i] - (now - ts).
 */
typedef struct _gtg_departure_node_ {
    time_t    ts;                                   /*!< when fetched, 0 when never */
    uint16_t  estimates[GTG_DEPARTURES_UPCOMING_MAX];  /*!< seconds from ts, soonest first */
    uint8_t   count;                                /*!< 0-GTG_DEPARTURES_UPCOMING_MAX in use */
    esp_err_t status;                               /*!< result of the last fetch */
    time_t    attempted;                            /*!< when last fetched, also when it failed */
} gtg_departure_node_t;

/**
 * A node per realtime target, in the order of dev_cfg. The targets are copied in, so a node
 * always belongs to the target next to it. When dev_cfg's targets change, the cache is cleared
 * and copied again, see gtg_cache_sync().
 *
 * Only used from one task, there is no lock.
 */
typedef struct _gtg_departure_cache_ {
    trafiklab_target_t   targets[GTG_DEV_CONFIG_MAX_TARGETS];
    gtg_departure_node_t nodes[GTG_DEV_CONFIG_MAX_TARGETS];
    size_t               len;
    uint32_t             version;    /*!< gtg_dev_config_get_targets_version() of the copy */
    bool                 synced;
} gtg_departure_cache_t;

/**
 * \brief Copies dev_cfg's targets into the cache when they changed, clearing every node
 *
 * \return true when the cache was cleared, the caller's index may point past the end
 */
bool gtg_cache_sync(gtg_departure_cache_t *cache);

/**
 * \brief Fetches the node at index if it is GTG_CACHE_MAX_AGE_S or older
 *
 * The fetch is for the target's stop group, so every other node on the same stop is filled by
 * the same request, with the same ts. A failed fetch keeps the nodes' old ts, so the next call
 * tries again.
 *
 * \return ESP_OK                fresh, fetched or not
 * \return ESP_ERR_INVALID_ARG   index is past the end
 * \return other                 from gtg_departures_fetch_upcoming(), stored in the nodes' status
 */
esp_err_t gtg_cache_refresh(gtg_departure_cache_t *cache, size_t index);

/**
 * \brief Fetches every node that is GTG_CACHE_MAX_AGE_S or older, one request per stop group
 */
void gtg_cache_refresh_all(gtg_departure_cache_t *cache);

/**
 * \brief Whether the node at index was fetched less than GTG_CACHE_MAX_AGE_S ago, so
 *        gtg_cache_refresh() makes no request
 */
bool gtg_cache_is_fresh(const gtg_departure_cache_t *cache, size_t index);

/**
 * \brief Seconds until the node at index is due for a background fetch, 0 when it is due
 *
 * Due GTG_CACHE_MAX_AGE_S after its last fetch, also a failed one, so a failing stop is tried
 * once a minute instead of over and over. A node never fetched is due right away.
 */
int32_t gtg_cache_seconds_until_due(const gtg_departure_cache_t *cache, size_t index);

/**
 * \brief The seconds from now to the coming departures of the target at index
 *
 * Departures that have left since the fetch are skipped. Nothing is fetched.
 *
 * \param *remaining  receives the seconds, soonest first
 *
 * \return the number of departures in remaining, 0 also when the fetch failed, see the status
 */
size_t gtg_cache_remaining(const gtg_departure_cache_t *cache, size_t index,
                           int32_t remaining[GTG_DEPARTURES_UPCOMING_MAX]);

/**
 * \brief Logs the target at index and its coming departures, counted from now
 */
void gtg_cache_log(const gtg_departure_cache_t *cache, size_t index);

#endif
