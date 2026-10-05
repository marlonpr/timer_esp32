# v6.23.10 — Stage B CPU0 blocker attribution

## Purpose
This revision follows v6.23.9 Stage A. It matches the historical failing workload and closes two monitor-coverage gaps.

## Historical workload
`CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT=y` on ESP01 and ESP02. The three natural ESP02 stalls (457/800/358 us callback lateness) came from network-silent RUNNING captures, so the first 30-minute Stage B run uses the same policy.

## Interrupt-level equivalence
The GPTimer sampler uses `CONFIG_ESP_TIMER_INTERRUPT_LEVEL` directly and requires the esp_timer ISR affinity to CPU0. This makes sampler and COMMIT observe the same CPU0 interrupt-level blockers. `wrong_core_callbacks` is also counted at runtime; Stage B validation requires it to remain zero.

## Complete boundary coverage
The 250 us sampler is not skipped around COMMIT. In the +/-400 us guard it still measures, attributes and retains >50 us events; only the GPIO32 probe pulse is suppressed. Therefore a long CPU0 mask cannot disappear solely because it occurred at a display boundary.

With period=250 us and threshold=50 us, any continuous CPU0 mask >=300 us necessarily delays at least one sampler alarm by >=50 us. Subject to the runtime checks (same level, CPU0 only, no ring overflow), a 358/457/800 us pre-entry COMMIT stall without a corresponding monitor event rules out one continuous CPU0 mask or one continuous same-level/higher-level ISR interval of that magnitude. It would then point to a different scheduling/arbitration mechanism rather than a single uninterrupted blocker.

## Two positive controls
- +15.5 s: task `lat_canary`, private CPU0 critical section, 600 us. Monitor event should name `lat_canary`.
- +16.5 s: ISR-dispatch esp_timer callback, same level/core class as COMMIT, busy-waits 500 us. Monitor must catch it, but `interrupted_task` is only the task underneath the ISR and is intentionally not interpreted as the ISR culprit.

The contrast demonstrates why task attribution is meaningful for a task-owned critical section but only contextual for a long ISR.

## Analyzer
No presentation offset is used (`0 us` on both devices), and this diagnostic build has a compile-time assertion that rejects any nonzero offset. Analyzer_v8 close-edge distortion below roughly 10 us remains a metrology limitation for fine inter-device phase, but it cannot hide a several-hundred-us natural stall. GPIO32 remains the latency-event probe on both boards. A future Analyzer_v9 should move COMMIT/REFRESH metrology to ESP32-S3 MCPWM hardware capture instead of perturbing the devices under test.

## Stage B
Run 1800 s with both serial logs and Analyzer capture. Do not add external AP load on the first run. Preserve the natural production-silent RUNNING network policy.
