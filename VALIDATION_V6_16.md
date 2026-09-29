# v6.16 short validation

## Wiring (one timer)

- DS3231 SQW / timer GPIO27 node -> Analyzer GPIO4
- Timer GPIO33 ISR publish marker -> Analyzer GPIO5
- Timer GPIO16 refresh-adoption marker -> Analyzer GPIO6
- Common ground
- GPIO17 is not used.

## Capture

1. Boot ESP01 and let RTC reach LOCKED.
2. Start analyzer `BEGIN|1|1`.
3. Keep the capture running for at least 60–90 seconds.
4. Run one 20 s countdown during that window so firmware dumps `SQW_TRACE` after END.
5. Save the complete ESP01 UART log and analyzer serial output.

The SQW ring retains the most recent 128 physical-second timestamps, so the countdown may happen near the end of the capture.

## Analysis

```powershell
python tools\analyze_sqw_latency.py ESP01.log analyzer.txt --threshold-us 3
```

The tool fits analyzer physical-SQW time against the ESP32 raw ISR timestamps using offset + linear rate. The constant ISR delay is absorbed by the offset; residual excursions are reported with the firmware's refresh phase and application network phase.

`network_phase` values:

- 0 = idle / no explicitly instrumented application network section
- 1 = `esp_wifi_sta_get_ap_info()`
- 2 = `sendto()`

A zero phase does not exclude internal Wi-Fi-driver activity.

## SYNC ingress fields

Each `SYNC_REPLY_TRACE` now has:

- `ip_input_us`: early lwIP IPv4-input timestamp
- `ip_input_to_t2_us`: delay from IPv4 input to application `recvfrom()` return
- existing T2/T3/sendto fields

This is not an RF timestamp. Driver/MAC time before lwIP remains unmeasured.


## v6.16b compile correction

The IPv4 hook compares `IPH_PROTO(&iphdr)` against lwIP/socket protocol constant `IPPROTO_UDP`. The original v6.16 package used the unavailable `IP_PROTO_UDP` name on ESP-IDF v6.0. No runtime behavior changed.
