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
        "flash_guard_diag.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_event esp_netif esp_timer esp_wifi nvs_flash spi_flash lwip factory_display rtc_discipline ${FACTORY_TIMER_EXTRA_REQUIRES}
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)


# v6.17 early receive timestamp (payload-fingerprint matched): use the upstream/ESP lwIP IPv4 input hook.
# The hook only timestamps UDP/5000 packets and always returns 0, so it never
# consumes or modifies traffic.
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_options(${lwip_lib} PRIVATE "-I${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(${lwip_lib} PRIVATE "-DESP_IDF_LWIP_HOOK_FILENAME=\"factory_lwip_hooks.h\"")
