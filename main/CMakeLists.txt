idf_component_register(
    SRCS
        "main.c"
        "src/gtg_soft_access_point.c"
        "src/gtg_wifi_station.c"
        "src/gtg_ntp.c"
        "src/gtg_https_server.c"
        "src/gtg_mdns.c"
        "lib/cJSON/cJSON.c"

    PRIV_REQUIRES
        esp_wifi
        esp_netif
        esp_https_server
        esp_littlefs
        lwip
        nvs_flash

    EMBED_TXTFILES
        "../cred/servercert.pem"
        "../cred/prvtkey.pem"

    INCLUDE_DIRS
        "."
        "inc/"
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
