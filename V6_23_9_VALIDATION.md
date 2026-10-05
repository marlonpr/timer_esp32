# v6.23.9 validation

## Build

```powershell
idf.py -B build-esp01-v6239 -D SDKCONFIG=sdkconfig.esp01 build
idf.py -B build-esp02-v6239 -D SDKCONFIG=sdkconfig.esp02 build
```

Expected boot lines on both boards:

```text
CPU0 latency monitor ready: gptimer=4kHz periodic_hw=1 period_us=250 threshold_us=50 guard_us=400 probe_gpio=32 canary_hold_us=600 ... intr_level=1
```

ESP01 presentation offset must report 0 us. ESP02 must report +50 us.
Neither board should announce the physical START-edge diagnostic on GPIO32.

## Wiring

- ESP01 GPIO32 -> Analyzer_v8 ch1 (`ESP01_SQW` label, repurposed)
- ESP01 GPIO33 -> ch2 `ESP01_COMMIT`
- ESP01 GPIO16 -> ch3 `ESP01_REFRESH`
- ESP02 GPIO32 -> ch4 (`ESP02_SQW` label, repurposed)
- ESP02 GPIO33 -> ch5 `ESP02_COMMIT`
- ESP02 GPIO16 -> ch6 `ESP02_REFRESH`
- common ground

Do not connect either DS3231 SQW line to Analyzer_v8 for this run.

## Stage A — 60 s monitor positive control

Run 60 s with Analyzer_v8.

Required on **both** devices:

- `CPU0_LATENCY_CANARY ... completed=1 ... duration_us` approximately 600 us.
- at least one `CPU0_LATENCY_EVENT` with `interrupted_task=lat_canary` and lateness >=300 us.
- GPIO32 Analyzer pulse(s) around +15.5 s, not near a COMMIT edge.
- event and COMMIT rings overflow=0.
- no unintended `CPU0_COMMIT_LATE` caused by the canary.
- normal countdown health: frame_not_ready=0.

Analyze:

```powershell
python tools\analyze_v6239_cpu0_latency.py ESP01_SERIAL.txt ESP02_SERIAL.txt Analyzer_v8.log --esp02-offset-us 50
```

`MONITOR_POSITIVE_CONTROL=PASS` is required before the long run.

## Stage B — 1800 s traffic-on run

Run 30 minutes with both boards and Analyzer_v8. Do not disable the diagnostic two-second heartbeat. Keep external AP load off for the first long run; this establishes each board's event rate under normal application traffic plus Wi-Fi management traffic.

Interpretation of a natural late COMMIT:

1. `CPU0_COMMIT_LATE` tells whether the delay is before entry and names the task that was current on CPU0 when the pending ISR finally entered.
2. `entry_to_marker_us` should remain ~1-2 us for the historical signature.
3. Nearby `CPU0_LATENCY_EVENT` records show whether CPU0 suffered broader interrupt latency away from exact boundaries and whether the same task repeatedly appears.
4. GPIO32 gives an independent physical timestamp for monitor events; GPIO33 remains the physical COMMIT truth.
5. ESP01 is the control for ESP02 event rate.

Do not interpret an arbitrary interrupted task under a long same-level ISR as proof that the task caused the ISR. Repeated, task-consistent attribution supports a critical-section owner; random/IDLE-like attribution with similar latency supports ISR-level interference.

## Stage C — only after Stage B

If natural stalls recur, repeat under controlled Wi-Fi/AP traffic. If needed, perform a separate esp_timer interrupt-level A/B only after the baseline culprit distribution is captured.
