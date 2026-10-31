#ifndef GTG_DEV_CONFIG_H_
#define GTG_DEV_CONFIG_H_

#include "gtg_trafiklab_ds.h"

typedef struct api_key {
    char   *key;
    size_t  len;
} api_key_t;

typedef struct _gtg_dev_config_ {
    char                    wifi_ssid[33];
    char                    wifi_pass[64];
    uint8_t                 wifi_is_connected;

    int64_t                 last_changed;

    trafiklab_target_list_t realtime_targets;

    api_key_t               trafiklab_api_key;
    api_key_t               google_maps_api_key;
} gtg_dev_config_t;

#endif
