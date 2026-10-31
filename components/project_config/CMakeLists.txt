# Header-only component with the project's hardware configuration, see include/project_config.h
idf_component_register(
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_i2c
        esp_driver_gpio
)
