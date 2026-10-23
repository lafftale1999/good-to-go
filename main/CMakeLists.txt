idf_component_register(
    SRCS
        "main.c"
        "src/gtg_soft_access_point.c"
        "src/gtg_wifi_station.c"
        "src/gtg_ntp.c"

    PRIV_REQUIRES
        esp_wifi
        esp_netif
        lwip
        nvs_flash

    INCLUDE_DIRS
        "."
        "inc/"
        "../cred/"
)
