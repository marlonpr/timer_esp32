if(IDF_TARGET STREQUAL "esp32")
    set(FACTORY_DISPLAY_SRCS
        "factory_display.cpp"
        "esp32/factory_display_backend_esp32.c"
    )
    set(FACTORY_DISPLAY_REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_timer
        esp_system
    )
elseif(IDF_TARGET STREQUAL "esp32s3")
    set(FACTORY_DISPLAY_SRCS
        "factory_display.cpp"
        "s3/factory_display_backend_s3.cpp"
    )
    set(FACTORY_DISPLAY_REQUIRES
        hub75
        esp_timer
    )
else()
    message(FATAL_ERROR "factory_display supports only ESP32 and ESP32-S3")
endif()

idf_component_register(
    SRCS ${FACTORY_DISPLAY_SRCS}
    INCLUDE_DIRS "include" "."
    REQUIRES ${FACTORY_DISPLAY_REQUIRES}
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
