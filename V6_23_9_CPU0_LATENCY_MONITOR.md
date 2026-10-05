# v6.23.9 — CPU0 interrupt-entry latency monitor

## What the v6.23.8 control proved

The deterministic GPIO32 mechanism run separated three mechanisms:

- b30: a 1001 us cache-off interval physically covered ESP02 COMMIT and did not move the COMMIT edge.
- b60: a 301 us private CPU0 critical section delayed COMMIT by about 209 us physically; firmware reported 227 us callback lateness and entry->marker remained 1 us.
- b90: the same private critical section on CPU1 physically covered COMMIT and did not move it.

The historical ESP02 stalls have the same pre-entry shape:

- START #1 b650: callback_lateness_us=457; entry->marker=1 us.
- START #2 b632: callback_lateness_us=800; entry->marker=1 us.
- START #2 b935: callback_lateness_us=358; entry->marker=1 us.

Therefore v6.23.9 measures CPU0 interrupt-entry latency. It does not assume whether the eventual cause is a critical section or another level-1 ISR.

## Why the older MARKER_GAP trace did not fire

On classic ESP32, visible publication is now performed by `BoundaryPublishTimerCallback()` in ISR context. The old `MARKER_GAP` / `COMMIT_DELAY` diagnostics describe the non-ESP32 task/flip path and do not measure time spent waiting before the ESP32 ISR callback enters. The retained `ISR_PUBLISH` trace is the authoritative COMMIT timing trace on this target.

## Monitor

Both ESP01 and ESP02 enable a GPTimer on CPU0:

- resolution: 1 MHz
- interrupt level: 1
- nominal sample spacing: 250 us (4 kHz)
- event threshold: 50 us
- COMMIT guard: +/-400 us
- ring: 256 latency events

The GPTimer uses hardware auto-reload at 250 us. It does **not** call `gptimer_set_alarm_action()` at 4 kHz. A DRAM software absolute schedule is maintained in local microseconds using `esp_timer_get_time()`. If CPU0 is masked across multiple hardware periods, the first delivered callback records the full `actual_local_us - expected_local_us` delay and advances the software schedule to the first future 250 us slot. This preserves >250 us latency measurements without adding a 4 kHz driver-control critical path.

Only >50 us events are retained. Each stores:

- GPTimer alarm and actual count
- lateness_us
- expected and actual local timestamp
- interrupted CPU0 task handle
- current exact display COMMIT target

The task inventory is captured in task context at run start, so handles are resolved after the run to task name/core/priority without doing name formatting inside the ISR.

## COMMIT attribution

Whenever the actual display ISR enters >=50 us late, the COMMIT callback records:

- boundary
- target local time
- callback entry
- marker begin/end
- entry->marker duration
- interrupted CPU0 task handle

This preserves the distinction:

- large callback lateness + normal entry->marker: pre-entry CPU0 blocking/interference
- normal callback lateness + large entry->marker: different mechanism / callback-path contention

## Positive control

A persistent `lat_canary` task pinned to CPU0 executes one private 600 us critical section at +15.5 s, halfway between display boundaries. Because the monitor period is 250 us, a >=300 us mask must produce at least one >50 us delayed sample. The delayed ISR should report `interrupted_task=lat_canary`.

The canary is deliberately away from COMMIT boundaries and is removed after diagnostic qualification.

## Boundary guard

The display scheduler publishes the exact local target every time it arms GPIO33 COMMIT. If a periodic GPTimer callback's nominal sample time lies within +/-400 us of that target, the callback takes only the minimal schedule-maintenance path: no task attribution, event retention, or GPIO32 pulse is performed. The hardware periodic interrupt itself is not reconfigured or stopped. This avoids a 4 kHz alarm-rearm path and keeps GPIO32 event markers away from GPIO33; the remaining monitor ISR cost is fixed and intentionally tiny.

## Analyzer GPIOs

For this diagnostic run, DS3231 SQW does not need to be connected to Analyzer_v8.

| Analyzer label | Timer signal |
|---|---|
| ESP01_SQW / ch1 | ESP01 GPIO32 CPU0-latency event probe |
| ESP01_COMMIT / ch2 | ESP01 GPIO33 |
| ESP01_REFRESH / ch3 | ESP01 GPIO16 |
| ESP02_SQW / ch4 | ESP02 GPIO32 CPU0-latency event probe |
| ESP02_COMMIT / ch5 | ESP02 GPIO33 |
| ESP02_REFRESH / ch6 | ESP02 GPIO16 |

The analyzer labels remain unchanged; the analysis tool interprets ch1/ch4 as latency probes.

## Analyzer close-edge correction

Analyzer_v8 shows about 5-6 us close-edge dead time / timestamp interaction when two independent COMMIT edges arrive within roughly 10 us. To keep pair metrology out of that regime:

- ESP01 presentation offset: 0 us
- ESP02 presentation offset: +50 us

This offset is applied only to the visual GPIO33 publication deadline. TimerTask/master START and countdown state remain unchanged. Subtract 50 us when analyzing ESP02-ESP01 physical phase.

## Traffic-on diagnostic

The production-style RUNNING heartbeat suppression is disabled in the ESP01/ESP02 diagnostic profiles. The existing two-second application heartbeat therefore exercises the Wi-Fi/network path during the run. Per-second STATE_CHANGE traffic remains disabled.

## Flash correlation

The validated main esp_flash OS wrapper remains enabled with threshold 0, but no flash GPIO probe or flash/critical injection is enabled. This allows a latency event caused by a real flash/cache interval to be identified and excluded from CPU0 culprit attribution, even though cache-off has already been ruled out as a COMMIT-delay mechanism.
