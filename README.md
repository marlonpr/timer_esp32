# Firmware 6.24.7-rc1 — GPIO33 active-LOW

GPIO33 is LOW while ON and HIGH while OFF. Startup signals for four seconds;
every end-of-repetition logo signals with four 500 ms LOW / 500 ms HIGH cycles.
STOP/RESET and completion return HIGH. COMMIT stays GPIO17. Keep the current
controller 6.23.17 and analyzer; rebuild/reflash the timer firmware only.
See [build and verification notes](../ACTIVE_LOW_fw6_24_7.md).

The entries below are historical and describe their original polarity.

# Firmware 6.24.6-rc1 — GPIO33 output and repetition logos

GPIO33 provides a four-second startup HIGH pulse and four 500 ms ON/OFF cycles
during each four-second logo. COMMIT diagnostics now use GPIO17; move analyzer
leads from GPIO33. Requires controller 6.23.17 for logo-enabled production runs.
See [build, wiring and timing notes](../LOGO_OUTPUT_fw6_24_6_ctl6_23_17.md).
The entries below are historical.

# Firmware 6.24.3 — three zone APs by default

Standard builds use FT-ZONE1/2/3 with idle roaming off. The supplied five-per-zone
map is for the analyzer bench. Map installation boards by physical position;
unequal counts such as 6/4/5 are supported by the deployment tools.
Shared-SSID strongest-AP roaming is optional and uses separately generated
profiles. Reconnect backoff remains enabled in both modes. Controller 6.23.6
and analyzer 10.1.1 are unchanged.
See [the current build and commissioning notes](../FIRMWARE_6_24_3_ZONE_DEFAULTS.md).

# Firmware 6.24.2 — guarded idle roaming and reconnect backoff (historical)

6.24.2 adds a Wi-Fi maintenance task, timing-control scan cancellation,
idle-only same-SSID roaming below -70 dBm with a 10 dB improvement requirement,
and 3-4 s reconnect pauses after three failures. Controller 6.23.6 and analyzer
10.1.1 are unchanged. Optional zone profiles keep each board on its zone SSID.
See [the 6.24.2 release notes](../FIRMWARE_6_24_2_WIFI_MAINTENANCE.md).

# Firmware 6.24.1 — multi-AP selection and bounded RTC startup

6.24.1 sets all-channel Wi-Fi scanning and signal-based AP ordering, probes the
DS3231 at 0x68, and bounds every RTC transaction to 100 ms. A failed RTC startup
returns to display/network startup, with RTC-dependent START blocked.
See [the release notes](../FIRMWARE_6_24_1_AP_RTC.md) for tests and deployment.
Controller 6.23.5 and analyzer 10.1.1 are unchanged.

# Firmware 6.24.0 — segmented repetition release candidate

See [the production implementation and validation notes](../SEGMENTED_REPETITIONS.md).
This extends the existing RTC scheduler; the 6.23.19 baseline is preserved.
Target compilation and physical fleet validation are required before deployment.
The historical build/preflight notes below remain useful for the unchanged paths.

# Factory timer firmware 6.23.19

6.23.19 appends `DiagnosticFlags` to STATUS (field 44, a two-digit hex byte).
It is computed from build options at compile time and logged once at boot;
`HealthFlags` retains its runtime meaning. Production fleet profiles report
`DiagnosticFlags=0x00`; the 500 us START test reports `0x04`. Task/ISR canaries,
timing-probe/cache/offset diagnostics and classic-ESP32 monitor-off profiles
are also explicitly identified. Observation-only COMMIT/refresh marker GPIOs
remain usable in qualification with zero flags.

STATUS now has 45 fields. Only in this layout, `RtcQueueDrops` (index 12) uses
hex instead of decimal, saving two maximum-width bytes. The maximum valid-width
packet, including `FF` reserved diagnostic bits, is exactly 511 bytes; no counter
is clipped. The formatter rejects negative synchronization epochs, matching the
existing controller wire contract. RUN_DIAG is unchanged at 21 fields / 243 bytes.
Update controller to 6.22.15 along with firmware; its preparation and final
STATUS gates reject nonzero or absent flags. The included diagnostic profiles
remain available, but cannot be started by the qualification controller.

## 6.23.18

6.23.18 caches both task inventories during CommandTask startup, after all
application and Wi-Fi tasks exist and before the UDP command socket opens.
The full `uxTaskGetSystemState()`/stack scan is no longer called from the display
ARM path or monitor run preparation. Task identities and per-task counters are
retained across runs; `vTaskGetInfo(..., pdFALSE, ...)` remains the lightweight
runtime-counter path where task-context display diagnostics use it.
Boot inventory logs include `phase=boot cached=1` and precede the command-socket
listening message. No STATUS or RUN_DIAG fields changed.

`CONFIG_FACTORY_START_TASK_TEST_DELAY_US` defaults to 0. Separate diagnostic
profiles `sdkconfig.esp01.start_delay_500us` through `sdkconfig.esp05.start_delay_500us`
inject 500 us after the START task reaches T*. Interrupts stay enabled; the
independent COMMIT ISR remains anchored to armed T*. STARTED/StartError uses the
actual delayed START timestamp. The enabled profile logs
`START_TASK_DELAY_TEST enabled delay_us=500 diagnostic_only=1` at boot.
Use a separate build directory and restore the ordinary profile for qualification.
See the bundle README for build commands and physical acceptance checks.

The new host test executes the actual classic-ESP32 display loop twice at each
of 0 and 500 us START lateness. It checks all 31 boundaries, exact 1000000 us
adjacent scheduled intervals, actual simulated COMMIT times and one inventory
scan before acceptance. A mutation that restores the post-boundary-0 actual
START epoch fails with an exact 1000500 us first interval. Monitor tests also
verify cached task names/counters survive repeated runs without another scan.
These tests model the clock/driver; hardware timing still requires the physical
30 s preflight. `PROJECT_VER` now correctly reports 6.23.18.

## 6.23.17

6.23.17 is 6.23.16 plus two review fixes. Folder and build-directory names are
unchanged.

- Sampler re-arm adds a near-deadline guard. When the next 251 us grid deadline
  is within 32 us of the ISR-entry
  count, the ISR re-reads the raw count and skips any deadline closer than 8 us,
  charging it as a missed period. The grid stays absolute and
  `callbacks + missed = deadlines` stays exact. `CPU0_MONITOR_WINDOW` reports
  `rearm_guard_skips`. Host test `monitor_rearm_test.cpp` models an
  equality-only comparator with 4 us pre-arm work; the 6.23.16 source fails it,
  6.23.17 passes. The test does not model the time spent programming the alarm.
- `RTC_TEMP_POST_RUN` adds `periodic_reads_during_run` (must be 0) and
  `suppressed_refreshes` (periodic DS3231 reads skipped while ARMED/RUNNING;
  roughly one per 64 s). The 30 s preflight can now prove suppression instead of
  relying on host tests. No STATUS or RUN_DIAG fields changed.

## 6.23.16

This revision addresses the new-build preflight blockers from 6.23.15a.

- Boundary 0 and every subsequent boundary use the same immutable armed
  disciplined epoch: `T_n = T* + n * 1000000`. No post-START anchor replaces it.
  Local deadlines can change as the RTC mapping is updated; scheduled
  disciplined timestamps must remain exactly one second apart.
- CPU0 monitoring begins in the accepted START_AT handler before temperature
  suppression and timer arming. The period is 251 us, threshold 50 us.
- The temperature mutex drains an in-flight DS3231 read, then suppresses periodic
  reads throughout ARMED/RUNNING. The finished handler performs a direct one-shot
  read and installs its cache/timestamp before permitting FINISHED STATUS. A
  failed read keeps the freshness barrier closed and can be retried. The
  temperature I2C transaction has a 100 ms timeout.
- STATUS remains 44 fields, conservatively 510 bytes in the 511-byte envelope.
  Window/task counters use a separate 21-field RUN_DIAG reply, at most 243 bytes.
- Actual COMMIT delay is `publish_marker_begin_us - target_local_us`; >=300 us
  delays are counted independently. Callback lateness and overlap are telemetry.
- Cumulative counts and worst delays continue after retained detail fills.
  The 30 s trace retains boundaries 0 through 30. Validation canary offsets
  remain relative to T*; fleet canaries remain disabled.

The free-running sampler uses `(start_count,end_count]` endpoints:

```text
ExpectedPeriods = floor(end_count / 251) - floor(start_count / 251)
                = 0, if elapsed < first_alarm_offset
                = 1 + floor((elapsed - first_alarm_offset) / 251), otherwise
```

RUN_DIAG carries the endpoints and first alarm offset so the controller can
reproduce this convention exactly. `DurationSeconds` is not used for sample
validation. A 30 s countdown with 5 s pre-start monitoring has about 35 s coverage.

Use the bundle README for PC commands and physical signoff. Per-board checker:

```powershell
python .\tools\validate_firmware_preflight_log.py .\ESP01.log --command-id YOUR_16_HEX_COMMAND_ID
```

It requires command-tagged publish records, monitor summary/window and
RTC_TEMP_POST_RUN. It rejects a 1 us epoch error, missing boundaries, incorrect
sample accounting, COMMIT >=300 us and a read that is not after finish and within
0.5 s. The physical Analyzer check is separate.

Host tests compile actual monitor/RTC source with IDF shims. ESP-IDF target builds
and hardware timing remain pending. Older V6_* documents/tests describe earlier
revisions; the bundle runner selects the current checks.
