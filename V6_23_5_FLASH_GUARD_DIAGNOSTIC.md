# v6.23.5 — temporary global flash-window diagnostic

This revision is a diagnostic-only follow-on to firmware v6.23.4. The validated
synchronization, START scheduling, RTC discipline, display publication, and RUNNING
network-silence algorithms are unchanged.

## Purpose

ESP02 showed rare publication stalls in two independent 30-minute runs:

- START #1: retained callback lateness about 457 us / POST_RUN worst 460 us.
- START #2: retained callback lateness 800 us at boundary 632 and 358 us at boundary 935.

Analyzer_v8 matched those individual events after subtracting the normal roughly
25–29 us callback-entry latency. v6.23.5 instruments the global ESP-IDF SPI-flash
guard so the next run can determine whether a cache-disabled flash window overlaps
one of these late publication callbacks.

## Diagnostic path

`main/flash_guard_diag.cpp` obtains the existing guard with `spi_flash_guard_get()`,
installs a wrapper with `spi_flash_guard_set()`, and delegates every operation to
the original guard.

The wrapper callbacks are `IRAM_ATTR` and touch only DRAM-resident counters/ring
records plus `esp_timer_get_time()`. They do not log, allocate, lock, or call NVS.
Flash windows >=100 us are retained in a 64-entry RAM ring. Total operation count,
total guard time, and maximum retained duration are also tracked.

At START_AT acceptance, after the command is already scheduled, the firmware saves
a diagnostic baseline and NVS statistics. At the existing asynchronous POST_RUN
dump it emits:

```text
FLASH_GUARD_DIAG ...
FLASH_GUARD_EVENT command=... sequence=... start_local_us=... duration_us=...
```

`FLASH_GUARD_EVENT.start_local_us` can be compared directly with the local target
and `callback_lateness_us` already retained by the display trace.


## GCC 15 note

The IRAM callback counters use explicit volatile load/store operations rather than
`++` or compound assignment.  GCC 15 compiling as GNU C++26 deprecates volatile
read-modify-write expressions, and this project treats that warning as an error.

## NVS probe limitation

NVS used/free/namespace counts are captured at ARM and POST_RUN. A changed count is
evidence that NVS storage changed during the interval, but unchanged counts do not
prove that no overwrite occurred. The global flash-guard trace is the authoritative
cache-disabled-window probe.

## Build profiles

The temporary diagnostic is enabled in `sdkconfig.esp01` and `sdkconfig.esp02` only:

```text
CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS=y
CONFIG_FACTORY_FLASH_GUARD_EVENT_THRESHOLD_US=100
```

Other device profiles are intentionally unchanged until the two-board diagnostic is
validated on target hardware.
