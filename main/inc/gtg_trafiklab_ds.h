#ifndef GTG_TRAFIKLAB_DATA_STRUCTURES_H_
#define GTG_TRAFIKLAB_DATA_STRUCTURES_H_

#include <stdio.h>
#include <stdint.h>

#include "gtg_departures.h"
#include "gtg_stop_search.h"



typedef struct _trafiklab_target_ {
    gtg_stop_t              stop;
    gtg_departure_option_t  departure;
} trafiklab_target_t;

typedef struct _trafiklab_target_list_ {
    trafiklab_target_t *list;
    size_t              len;
    size_t              size;
} trafiklab_target_list_t;

#endif
