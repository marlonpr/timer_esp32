# v6.23.10 validation summary

## Stage A findings carried forward
- v6.23.9 4 kHz CPU0 sampler positive control passed on both boards.
- Task canary correctly attributed a task-owned CPU0 critical section to `lat_canary`.
- Historical ESP02 457/800/358 us COMMIT stalls are pre-entry events: entry-to-marker remains 1-2 us.
- ESP01 IDF flash counters already observed 22 writes / 19242 us / 2112 bytes before Stage A ARM; ESP02 observed zero writes since boot in the same Stage A capture.
- Analyzer_v8 has close-edge distortion below roughly 10 us; no DUT presentation offset is used in v6.23.10.

## v6.23.10 changes
- Historical failing workload restored: RUNNING heartbeat suppression ON for ESP01 and ESP02.
- GPTimer sampler interrupt priority derives directly from `CONFIG_ESP_TIMER_INTERRUPT_LEVEL`.
- Build requires esp_timer ISR affinity to CPU0.
- Runtime `wrong_core_callbacks` must stay zero.
- Boundary guard no longer skips latency measurement. Near COMMIT it suppresses only GPIO32 pulse output; event retention remains active.
- Task canary: +15.5 s, 600 us private CPU0 critical section.
- Same-level ISR canary: +16.5 s, 500 us `ESP_TIMER_ISR` busy wait.
- Presentation offset fixed at 0 us for both boards and rejected at compile time if nonzero.
- Stage B analysis script classifies TASK_CANARY / ISR_CANARY / NATURAL events and validates same-level/same-core monitor conditions.

## Static/source validation performed here
- `test_v62310_stage_b_source.py`: 43/43 PASS.
- Existing v6.23.2 frame-boundary contract: PASS.
- Existing v6.23.3 fleet-health contract: PASS.
- Existing v6.23.4 qualification-guard contract: PASS.
- Existing v6.23.5+ flash diagnostic invariants: PASS.
- Existing v6.23.1 SQW source contract: PASS.
- Fit residual arithmetic test: PASS.
- GCC 15 duplicate-IRAM declaration regression: 3/3 PASS.
- Python syntax compilation for v6.23.10 analyzer/tests: PASS.
- Synthetic Stage B replay with both canaries: `STAGE_B_MONITOR_VALIDATION=PASS`.

## Not performed here
An ESP-IDF target build/flash was not available in this environment. Build both profiles in the user's ESP-IDF v6.0 Windows environment before the hardware run.

## Stage B first run
- 1800 seconds.
- Heartbeats suppressed, matching the historical stall runs.
- No external AP load on the first run.
- Analyzer_v8 acceptable for the several-hundred-us stall question; fine sub-10-us pair metrology remains limited.
- Save complete UART logs for ESP01/ESP02, Analyzer log, controller health CSV, and controller TX CSV.
