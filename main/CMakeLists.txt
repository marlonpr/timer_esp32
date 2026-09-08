idf_component_register(
    SRCS
        "main.cpp"
        "command_processor.cpp"
        "countdown_timer.cpp"
        "protocol_codec.cpp"
        "network_policy.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_netif esp_timer esp_wifi nvs_flash lwip
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
