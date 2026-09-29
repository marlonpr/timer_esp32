# v6.17 short validation

## Purpose

Determine whether the large v6.16 ESP-side SQW timestamp RMS was caused by the continuously updated HUB75 refresh-correlation telemetry.

## Wiring

Use Analyzer v7 unchanged:

- Analyzer GPIO4 <- DS3231 SQW / timer GPIO27 node
- Analyzer GPIO5 <- timer GPIO33 commit marker
- Analyzer GPIO6 <- timer GPIO16 refresh-adoption marker
- common ground

GPIO17 is not used.

## Procedure

1. Flash v6.17 on ESP01.
2. Start UART capture and Analyzer v7.
3. Allow RTC discipline to reach LOCKED.
4. Keep capturing for at least 60–90 seconds.
5. During the capture run one 20-second countdown so GPIO33 provides a 21-edge clock anchor (START plus 20 boundaries).
6. Save UART and analyzer logs.
7. Run:

```
python tools/analyze_sqw_latency.py ESP01.log Analyzer_v7.log --threshold-us 3
```

## Main comparison

v6.16 observed ESP-side SQW interval RMS around 6 us while the analyzer physical SQW stayed around 0.5 us RMS.

If the v6.16 regression was instrumentation-induced, v6.17 should move the firmware SQW interval statistics back toward the earlier v6.15 range (~1–1.6 us rather than ~6 us).

The exact rate-fit RMS is not the primary short-test metric while the ESP oscillator is warming; thermal curvature can dominate that 128-second regression residual.

## Pass/fail for moving on

Proceed to thermal settling / long qualification if:

- no SQW queue drops or inferred missing edges;
- raw SQW timestamp interval RMS is back near the pre-v6.16 class and has no persistent new multimodal delay;
- GPIO33 START/boundary publication remains in the tens-of-microseconds class with frame_not_ready=0;
- early `ip_input_us` matching remains populated for the SYNC samples.

Do not change the production sync estimator from this short run. The early ingress timestamp and sendto duration remain diagnostic fields until the full per-sample controller T4/RTT analysis is available.
