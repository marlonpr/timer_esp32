# v6.18-A validation

## Goal

Determine whether removing the SQW trace ring restores the ESP-side SQW timestamp-repeatability statistics toward the v6.15 range.

## Build

```powershell
idf.py -B build-esp01 -D SDKCONFIG=sdkconfig.esp01 build
```

Confirm generated config:

```powershell
Select-String .\build-esp01\config\sdkconfig.h -Pattern "FACTORY_RTC_SQW_CORRELATION_TRACE"
```

Expected: the symbol is absent / disabled.

## Wiring

Use Analyzer v7 unchanged:

- analyzer GPIO4 <- physical DS3231 SQW / ESP32 GPIO27 node
- analyzer GPIO5 <- ESP32 GPIO33 COMMIT
- analyzer GPIO6 <- ESP32 GPIO16 REFRESH
- common ground

## Run

1. Power/reboot ESP01.
2. Start analyzer capture.
3. Let the device run for 60–90 seconds.
4. Run one normal 20-second countdown during the capture.
5. Save UART and analyzer logs.

No `SQW_TRACE_BEGIN/END` block is expected after the countdown.

## Primary comparison

Use `RTC_QUAL` only:

- `sqw_period_rms_us`
- `sqw_period_p2p_us`
- `queue_drops`
- `inferred_missing`

Compare against:

- v6.15: roughly 1–1.6 us SQW-period RMS late in the run.
- v6.17: roughly 7–9 us RMS with p2p up to 47 us.

Interpretation:

- If v6.18-A returns near the v6.15 range, the raw SQW ring / ISR critical section was the perturbation.
- If v6.18-A remains near v6.17, proceed to v6.18-B and remove the lwIP ingress hook while keeping the display ISR path unchanged.

The external analyzer SQW remains useful to confirm the DS3231 waveform itself stays clean, but v6.18-A intentionally has no firmware SQW trace for per-edge latency fitting.
