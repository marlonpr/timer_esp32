# v6.23.5 validation performed in this environment

## Source-contract checks

- existing v6.23.3/v6.23.4 health telemetry tests: PASS
- existing v6.23.4 continuity guard tests: PASS
- new v6.23.5 flash-guard diagnostic source-contract checks: 14/14 PASS
- Python compile checks across firmware tools: PASS

The source-contract test verifies that:

- the original SPI flash guard is obtained and delegated to;
- wrapper callbacks are `IRAM_ATTR`;
- the wrapper is installed after NVS initialization;
- START_AT only takes a diagnostic baseline after scheduling is accepted;
- flash/NVS diagnostics are dumped only from the existing post-run task;
- ESP01/ESP02 diagnostic profiles enable the feature;
- the existing display publication callback remains `IRAM_ATTR` with `ESP_TIMER_ISR` dispatch.


## GCC 15 / C++26 compatibility fix

The first Windows ESP-IDF v6.0 target build exposed `-Werror=volatile` in GCC
15.2 for the diagnostic-only counter expressions `++s_total_operations` and
`s_total_duration_us += duration_us`.  Both were replaced with explicit volatile
load/store operations.  This changes no diagnostic semantics and avoids deprecated
volatile read-modify-write syntax under C++26.  The v6.23.5 source-contract suite
now checks that those expressions are not reintroduced.

## Required target build

ESP-IDF is not installed in this execution environment. Build both validated classic
ESP32 profiles on the Windows ESP-IDF v6.0 machine before flashing:

```powershell
idf.py -B build-esp01-v6235 -D SDKCONFIG=sdkconfig.esp01 build
idf.py -B build-esp02-v6235 -D SDKCONFIG=sdkconfig.esp02 build
```

After build, confirm the generated configuration contains:

```text
CONFIG_ESP_TIMER_IN_IRAM=y
CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS=y
CONFIG_FACTORY_FLASH_GUARD_EVENT_THRESHOLD_US=100
```

## Hardware proof target

Run a 30-minute Analyzer_v8 capture. For every retained late publication event,
compare `callback_lateness_us` with any overlapping:

```text
FLASH_GUARD_EVENT ... start_local_us=... duration_us=...
```

A time overlap would strongly identify a cache-disabled flash window as the cause.
If late publication events occur without any overlapping flash window, investigate
interrupt masking / long critical sections next.
