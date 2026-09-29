# v6.16 — SQW correlation trace + early IPv4 ingress timestamp

## Display path

v6.15b START-through-ISR and the one-second ISR publication path are unchanged.

## SQW diagnostic change

The GPIO17 SQW ISR marker is removed completely. The SQW ISR now does this in order:

1. `esp_timer_get_time()` — the disciplined timestamp.
2. Snapshot diagnostic metadata (refresh phase and application network phase).
3. Queue the edge for the estimator.

No diagnostic GPIO write is performed in the discipline ISR.

A 128-record ring stores:

- raw local SQW timestamp
- ISR core
- packed HUB75 refresh phase: frame / bitplane / row / stage
- application network phase (`IDLE`, `WIFI_QUERY`, `SENDTO`)

The trace is dumped after a countdown as `SQW_TRACE ...` records. Pair the firmware timestamps with the analyzer's physical DS3231 SQW rising edges offline; a linear offset/rate fit removes the independent analyzer clock and leaves edge-service residuals.

## Early forward-leg timestamp

A custom lwIP IPv4 input hook timestamps UDP packets destined for port 5000 near the beginning of `ip4_input()`. The hook does not consume or alter packets. The application matches that timestamp to the later `recvfrom()` result using source address, source port and UDP payload length.

New fields:

- `ip_input_us`
- `ip_input_to_t2_us` in `SYNC_REPLY_TRACE`
- `ip_input_to_recv_us` in `STATUS_PATH`

This is an **IP-stack ingress timestamp**, not an RF/MAC timestamp. Wi-Fi driver time before `ip4_input()` remains outside the measurement.

## Qualification use

Keep the correlation trace enabled for the short 60–90 s diagnostic. Disable `CONFIG_FACTORY_RTC_SQW_CORRELATION_TRACE` for the final 30-minute qualification after the excursion source is understood.
