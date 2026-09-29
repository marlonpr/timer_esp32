idf_component_register(
    SRCS
        "ds3231.c"
        "rtc_discipline.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_timer
)
