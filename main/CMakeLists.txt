set(FACTORY_TIMER_EXTRA_REQUIRES "")
if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND FACTORY_TIMER_EXTRA_REQUIRES esp_driver_tsens)
endif()

idf_component_register(
    SRCS
        "main.cpp"
        "command_processor.cpp"
        "countdown_timer.cpp"
        "protocol_codec.cpp"
        "network_policy.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_event esp_netif esp_timer esp_wifi nvs_flash lwip factory_display ${FACTORY_TIMER_EXTRA_REQUIRES}
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
