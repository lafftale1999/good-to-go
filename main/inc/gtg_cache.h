#ifndef GTG_CACHE_H_
#define GTG_CACHE_H_

#include "gtg_dev_config.h"
#include <stdint.h>

/**
 * The cache shall contain information no older than 1 minute. If the information
 * is older than 1 minute, it should be re-fetch.
*/

typedef struct _gtg_departure_node_ {
    time_t  ts;
    uint8_t estimates[3];
    uint8_t active;
} gtg_departure_node_t;

typedef struct _gtg_departure_cache_ {
    gtg_departure_node_t nodes[GTG_DEV_CONFIG_MAX_TARGETS];
} gtg_departure_cache_t;



#endif

