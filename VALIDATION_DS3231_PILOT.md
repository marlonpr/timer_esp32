# DS3231 pilot validation

Validation performed on the merged source package:

- Firmware-host CMake build: PASS
- `factory_timer_core_debug_0`: PASS
- `factory_timer_core_debug_1`: PASS
- Added dual-clock countdown test: PASS
- `ds3231.c`: GNU17 syntax check with `-Wall -Wextra -Werror`: PASS
- `rtc_discipline.c`: GNU17 syntax check with `-Wall -Wextra -Werror`: PASS
- `factory_display.cpp`: GNU++17 syntax check with `-Wall -Wextra -Werror`: PASS
- Static pin-map check: no overlap between current HUB75 pins and
  SDA=21 / SCL=22 / SQW=27 / configured START diagnostic pin
- Static source check: old raw `local_start + N*1,000,000` post-START display
  scheduling pattern removed

The validation environment does not contain an ESP-IDF installation, so a complete
`idf.py build` for the ESP32 and ESP32-S3 targets was not claimed here. The RTC component
uses the ESP-IDF 6.x `esp_driver_i2c` dependency required by `driver/i2c_master.h`.
