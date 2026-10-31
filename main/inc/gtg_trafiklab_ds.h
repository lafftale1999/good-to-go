#ifndef GTG_TRAFIKLAB_DATA_STRUCTURES_H_
#define GTG_TRAFIKLAB_DATA_STRUCTURES_H_

#include <stdio.h>
#include <stdint.h>

typedef struct _trafiklab_target_ {
    char stop_id[10];
    char area_name[50];
    char stop_name[50];
    char *transport_modes[10];
    char line_designation[10];
    char destination[100];
} trafiklab_target_t;

typedef struct _trafiklab_target_list_ {
    trafiklab_target_t *list;
    size_t              len;
    size_t              size;
} trafiklab_target_list_t;

#endif
