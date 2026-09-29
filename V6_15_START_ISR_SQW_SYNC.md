# v6.15 START ISR + SQW/SYNC diagnostics

Changes from v6.14:

- START frame (boundary 0) is pre-rendered and published by the same ISR-dispatched esp_timer path used for boundaries 1..N.
- ISR_PUBLISH trace now includes boundary=0 for START; frame_not_ready covers START as well.
- GPIO17 toggles from the DS3231 SQW ISR after the raw esp_timer timestamp. For a one-device diagnostic, compare the physical SQW rising edge with GPIO17 to measure ISR service latency.
- RTC_QUAL adds SQW ISR core, rolling 64-interval mean/RMS/p2p, last period, and marker cost.
- SYNC reply tracing is retained across changing sync IDs; a record matching a received SYNC_SET is flagged selected=1.
- Ordinary (non-artificial-delay) SYNC replies pre-build a fixed-width T3 field and patch T3 immediately before sendto, reducing formatting-after-T3 delay. Positive-control delayed replies retain the previous semantics.

## Suggested SQW diagnostic wiring (one device)

- DS3231 SQW / ESP GPIO27 -> analyzer channel A (capture rising edges)
- ESP GPIO17 -> analyzer channel B (SQW ISR service marker)
- ESP GPIO33 -> optional analyzer channel C (display publish)
- ESP GPIO16 -> optional analyzer channel D (refresh adoption)
- common ground

The GPIO17 marker is diagnostic-only and should be removed/disabled for the final production qualification once SQW service latency is understood.

## Important limitation

This build does not yet add an lwIP/IP-input hardware/network-stack receive timestamp ahead of recvfrom(). The existing T2 remains the socket-return timestamp. A lower-level forward-leg hook should be added only after choosing an IDF-supported, minimally intrusive hook for classic ESP32.
