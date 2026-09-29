# v6.14 validation performed in this environment

- Firmware host tests: 2/2 passed (`factory_timer_core_debug_0`, `factory_timer_core_debug_1`).
- `factory_display.cpp`: C++17 syntax checked with ESP-IDF/FreeRTOS API stubs for the classic ESP32 configuration.
- `factory_display_backend_esp32.c`: GNU C11 syntax checked with API/register stubs.
- Python analysis tools: `py_compile` passed.
- `CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD=y` is present in the device sdkconfig profiles.
- Classic profiles ESP01/ESP02/ESP04 enable GPIO16 refresh-adoption diagnostics; S3 profiles leave it disabled.
- Network policy remains `192.168.0.0/24`, gateway `192.168.0.1`.

A real ESP-IDF v6.0 target build was not available in this environment and must be run before flashing. The Windows controller source could not be built here because the .NET SDK is unavailable.
