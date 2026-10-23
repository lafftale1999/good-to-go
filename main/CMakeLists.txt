idf_component_register(
    SRCS
        "main.c"
        "src/gtg_soft_access_point.c"

    PRIV_REQUIRES
        esp_wifi
        nvs_flash

    INCLUDE_DIRS
        "."
        "inc/"
)
