# v6.23.7 — esp_flash OS targeted positive control

Firmware timing architecture remains v6.23.4. This is diagnostic firmware for ESP01/ESP02 only.

## Why v6.23.7 exists

The v6.23.6 180 s positive-control run completed 120 successful `nvs_set_u32` + `nvs_commit` operations, and NVS used/free counts changed, but the legacy/global `spi_flash_guard_set` wrapper reported zero operations for every write. In ESP-IDF v6.0, main-flash operations use `esp_flash_default_chip->os_func->start/end`; v6.23.7 wraps that actual path instead.

The v6.23.6 phase sweep also missed the physical COMMIT boundary. The closest pair was sequence 98 ending about 2.3 ms before a boundary and sequence 99 starting about 1 ms after the next boundary once the RTC-disciplined local period and task dispatch latency were included.

## v6.23.7 changes

- Wrap `esp_flash_default_chip->os_func` by copying the installed `esp_flash_os_functions_t` and replacing only `start` and `end`.
- The wrapper delegates every original callback and records only IRAM/DRAM state while cache is disabled.
- Enable `CONFIG_SPI_FLASH_ENABLE_COUNTERS=y` on ESP01/ESP02 as an independent flash-write count/time check.
- ESP01 remains an unstressed control.
- ESP02 performs three real NVS updates targeted at disciplined countdown boundaries 30, 60 and 90.
- The value is staged with `nvs_set_u32()` several milliseconds early. `nvs_commit()` is launched about 500 us before the exact disciplined local boundary.
- No NVS API or logging runs in the countdown ISR or esp_timer callback.

## Test

Build/flash ESP01 and ESP02, attach Analyzer_v8, then run one 3:00 countdown.

Expected ESP02 post-run records:

```text
FLASH_OS_DIAG ... operations=>0 ... flash_counters_valid=1 write_count_delta=>0 ...
INDUCED_NVS_DIAG ... mode=targeted boundaries=30,60,90 ... requested=3 completed=3 ...
INDUCED_NVS_EVENT ... target_boundary=30 ... commit_begin_minus_target_us≈-500 ... flash_operations=>0 ...
INDUCED_NVS_EVENT ... target_boundary=60 ...
INDUCED_NVS_EVENT ... target_boundary=90 ...
```

Run:

```powershell
python tools\analyze_targeted_nvs_positive_control.py ESP02_SERIAL.txt Analyzer_v8.log --device ESP02
```

The probe positive control requires successful NVS commits, non-zero per-write flash OS operations, non-zero aggregate flash OS operations, and non-zero official flash write counters.

The mechanism test requires at least one recorded flash/cache-disabled window to contain a target COMMIT boundary. Compare the corresponding Analyzer residual and firmware callback lateness.
