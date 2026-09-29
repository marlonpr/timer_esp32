# v6.11 Marker-gap diagnostic

Purpose: locate the multi-millisecond interval seen between the classic ESP32
software frame-commit timestamp and the GPIO33 presentation marker without
making the marker atomic first.

This build is based on the v6.9 baseline scheduler. It keeps:

- DS3231 discipline and nanosecond anchor.
- RTC LOCKED participation gate.
- UDP RX_NEAR_BOUNDARY tracing.
- Existing non-atomic GPIO33 toggle ordering.
- Factory LAN policy 192.168.0.0/24, gateway 192.168.0.1.

It adds:

1. Three timestamps around every visible commit:
   - `flip_commit_us`: immediately after `factory_display_backend_flip()`.
   - `toggle_before_us`: immediately before the GPIO33 toggle call.
   - `toggle_after_us`: immediately after the GPIO33 toggle call.

2. A `MARKER_GAP` diagnostic whenever
   `toggle_after_us - flip_commit_us > 100 us`.

3. FreeRTOS run-time statistics (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`)
   using the ESP Timer clock. A task inventory is taken once per armed run,
   before the precision timing phase. Runtime counters are sampled only after
   each GPIO marker, so the sampling cannot manufacture the marker gap being
   measured.

4. On an anomalous marker gap, `MARKER_GAP_TASK` lines report task run-time
   deltas between the previous marker snapshot and the current marker snapshot.
   This interval is approximately one second. It is deliberately not described
   as an exact micro-gap profiler; the exact location of the delay comes from
   the three timestamps, while the task deltas identify which core-0 task had
   an unusual amount of CPU time in the same boundary interval.

Example:

```
W (...) factory_display: MARKER_GAP boundary=7 ... flip_to_before_us=3620 toggle_call_us=3 flip_to_after_us=3623 runtime_interval_us=1000011 ...
W (...) factory_display: MARKER_GAP_TASK boundary=7 rank=1 task=IDLE0 core=0 priority=0 runtime_delta_us=...
W (...) factory_display: MARKER_GAP_TASK boundary=7 rank=2 task=wifi core=0 priority=23 runtime_delta_us=4010
```

Interpretation:

- Large `flip_to_before_us`, tiny `toggle_call_us`: the display task was delayed
  after the flip timestamp and before entering the GPIO write.
- Tiny `flip_to_before_us`, large `toggle_call_us`: the delay happened while
  executing the toggle path (or through a preemption inside that call).
- Large `flip_commit_lateness_us`: the actual software commit itself missed the
  boundary, independent of marker timing.
- A task whose one-second runtime delta rises by approximately the same amount
  as the marker gap is the primary scheduler suspect. Compare against normal
  adjacent boundary intervals rather than treating one delta in isolation.

This is a diagnostic-only build. Run-time stats add scheduler bookkeeping, so
return to the production/baseline build after the culprit is identified.

## ESP-IDF 6.0 build fix

Runtime task inventory obtains task affinity with `xTaskGetCoreID(task_handle)` instead of reading the optional `TaskStatus_t::xCoreID` field. This avoids a dependency on `CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID` and is compatible with the ESP-IDF 6.0 FreeRTOS API.
