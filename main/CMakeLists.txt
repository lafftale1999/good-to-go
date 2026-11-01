idf_component_register(
    SRCS
        "main.c"
        "src/api/gtg_api_keys.c"
        "src/api/gtg_departures.c"
        "src/api/gtg_stop_search.c"
        "src/api/gtg_trafiklab_ds.c"
        "src/app/gtg_app.c"
        "src/app/gtg_cache.c"
        "src/app/gtg_dev_config.c"
        "src/app/gtg_events.c"
        "src/driver/gtg_button.c"
        "src/driver/gtg_lcd.c"
        "src/network/gtg_https_server.c"
        "src/network/gtg_mdns.c"
        "src/network/gtg_ntp.c"
        "src/network/gtg_soft_access_point.c"
        "src/network/gtg_wifi.c"
        "src/network/gtg_wifi_station.c"
        "src/utils/gtg_sleep.c"
        "lib/cJSON/cJSON.c"

    PRIV_REQUIRES
        esp_wifi
        esp_netif
        esp_https_server
        esp_http_client
        mbedtls
        esp_littlefs
        lwip
        nvs_flash
        esp_timer
        esp_driver_gpio
        project_config
        lcd_screen

    EMBED_TXTFILES
        "../cred/servercert.pem"
        "../cred/prvtkey.pem"

    INCLUDE_DIRS
        "."
        "inc/"
        "inc/api"
        "inc/app"
        "inc/driver/"
        "inc/network"
        "inc/utils"
        "lib/cJSON/"
        "../cred/"
)

# Gzip the web files from web/ and flash them to the "www" LittleFS partition
idf_build_get_property(python PYTHON)

set(WEB_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../web)
set(WEB_OUT_DIR ${CMAKE_BINARY_DIR}/www)
set(WEB_STAMP ${CMAKE_BINARY_DIR}/www.stamp)
set(WEB_GZIP_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gzip_web.py)

file(GLOB_RECURSE WEB_SRC_FILES CONFIGURE_DEPENDS ${WEB_SRC_DIR}/*)

add_custom_command(
    OUTPUT ${WEB_STAMP}
    COMMAND ${python} ${WEB_GZIP_SCRIPT} ${WEB_SRC_DIR} ${WEB_OUT_DIR} ${WEB_STAMP}
    DEPENDS ${WEB_SRC_FILES} ${WEB_GZIP_SCRIPT}
    COMMENT "Compressing web files"
    VERBATIM
)

littlefs_create_partition_image(www ${WEB_OUT_DIR} FLASH_IN_PROJECT DEPENDS ${WEB_STAMP})
