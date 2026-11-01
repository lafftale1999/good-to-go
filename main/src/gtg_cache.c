#include "gtg_cache.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "cache";

bool gtg_cache_sync(gtg_departure_cache_t *cache)
{
    uint32_t version = gtg_dev_config_get_targets_version();
    if (cache->synced && cache->version == version) {
        return false;
    }

    memset(cache->nodes, 0, sizeof(cache->nodes));

    size_t len = 0;
    if (gtg_dev_config_get_targets(cache->targets, GTG_DEV_CONFIG_MAX_TARGETS, &len) != ESP_OK) {
        len = 0;
    }
    cache->len = len;
    cache->version = version;
    cache->synced = true;

    ESP_LOGI(TAG, "cleared, %u targets", (unsigned)len);

    return true;
}

static bool is_fresh(const gtg_departure_node_t *node, time_t now)
{
    return node->ts != 0 && node->status == ESP_OK && difftime(now, node->ts) < GTG_CACHE_MAX_AGE_S;
}

/* Fetches the stop of the target at index for every target on the same stop */
static esp_err_t fetch_stop(gtg_departure_cache_t *cache, size_t index)
{
    const char *stop_id = cache->targets[index].stop.id;

    size_t indices[GTG_DEV_CONFIG_MAX_TARGETS];
    gtg_departure_option_t departures[GTG_DEV_CONFIG_MAX_TARGETS];
    gtg_departures_upcoming_t upcoming[GTG_DEV_CONFIG_MAX_TARGETS];
    size_t count = 0;

    for (size_t i = 0; i < cache->len; i++) {
        if (strcmp(cache->targets[i].stop.id, stop_id) == 0) {
            indices[count] = i;
            departures[count] = cache->targets[i].departure;
            count++;
        }
    }

    time_t fetched_at = 0;
    esp_err_t err = gtg_departures_fetch_upcoming(stop_id, departures, count, upcoming, &fetched_at);

    for (size_t i = 0; i < count; i++) {
        gtg_departure_node_t *node = &cache->nodes[indices[i]];
        node->status = err;

        if (err == ESP_OK) {
            node->ts = fetched_at;
            node->count = upcoming[i].count;
            memcpy(node->estimates, upcoming[i].seconds, sizeof(node->estimates));
        }
    }

    ESP_LOGI(TAG, "fetched %s for %u targets: %s", cache->targets[index].stop.name, (unsigned)count,
             esp_err_to_name(err));

    return err;
}

esp_err_t gtg_cache_refresh(gtg_departure_cache_t *cache, size_t index)
{
    if (index >= cache->len) {
        return ESP_ERR_INVALID_ARG;
    }

    if (is_fresh(&cache->nodes[index], time(NULL))) {
        ESP_LOGI(TAG, "target %u is fresh, no request", (unsigned)index);
        return ESP_OK;
    }

    return fetch_stop(cache, index);
}

void gtg_cache_refresh_all(gtg_departure_cache_t *cache)
{
    /* a fetch fills every node on its stop, so the later ones on a fetched stop are fresh */
    for (size_t i = 0; i < cache->len; i++) {
        gtg_cache_refresh(cache, i);
    }
}

void gtg_cache_log(const gtg_departure_cache_t *cache, size_t index)
{
    if (index >= cache->len) {
        return;
    }

    const trafiklab_target_t *target = &cache->targets[index];
    const gtg_departure_node_t *node = &cache->nodes[index];

    char where[GTG_STOP_NAME_MAX_LEN + GTG_DEPARTURE_LINE_MAX_LEN + GTG_DEPARTURE_MODE_MAX_LEN
               + GTG_DEPARTURE_DEST_MAX_LEN + 8];
    snprintf(where, sizeof(where), "%s, %s %s %s", target->stop.name, target->departure.mode,
             target->departure.line, target->departure.destination);

    if (node->status != ESP_OK || node->ts == 0) {
        ESP_LOGW(TAG, "[%u/%u] %s: no departures, the fetch failed: %s", (unsigned)index + 1,
                 (unsigned)cache->len, where, esp_err_to_name(node->status));
        return;
    }

    if (node->count == 0) {
        ESP_LOGI(TAG, "[%u/%u] %s: no departures within %d min", (unsigned)index + 1,
                 (unsigned)cache->len, where, GTG_DEPARTURES_UPCOMING_WINDOW_S / 60);
        return;
    }

    /* counted from now, the node can be up to GTG_CACHE_MAX_AGE_S old */
    long age = (long)difftime(time(NULL), node->ts);

    char times[GTG_DEPARTURES_UPCOMING_MAX * sizeof("60 min, ")] = "";
    size_t used = 0;
    for (size_t i = 0; i < node->count && used < sizeof(times); i++) {
        long seconds = (long)node->estimates[i] - age;
        if (seconds < 0) {
            seconds = 0;
        }

        const char *sep = i > 0 ? ", " : "";
        int n = seconds < 60
            ? snprintf(times + used, sizeof(times) - used, "%snow", sep)
            : snprintf(times + used, sizeof(times) - used, "%s%ld min", sep, seconds / 60);
        if (n < 0) {
            break;
        }
        used += (size_t)n;
    }

    ESP_LOGI(TAG, "[%u/%u] %s: %s (fetched %ld s ago)", (unsigned)index + 1, (unsigned)cache->len,
             where, times, age);
}
