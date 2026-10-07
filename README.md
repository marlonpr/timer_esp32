# Factory timer firmware 6.23.18

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
