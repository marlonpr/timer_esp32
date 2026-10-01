# v6.21 RTC-qualified START

v6.21 promotes RTC qualification from a serial-log observation into a production START prerequisite.

## Firmware gate
START and START_AT are accepted only when all of the following are true:

- RTC discipline state is `LOCKED`.
- `fit_point_count >= 64`.
- `fit_rms_us <= 3.0`.
- `isr_queue_drops == 0`.
- DS3231 temperature is valid.

The gate is independent of the Windows controller, so a direct/manual START cannot bypass RTC readiness.

## STATUS extension
Firmware now emits:

`FCT2|STATUS|device|command|state|remaining|rssi|channel|bssid|rtc_state|fit_points|fit_rms_us|queue_drops|temp_valid`

Example:

`FCT2|STATUS|ESP01|0000000000000000|READY|0|-48|6|AA:BB:CC:DD:EE:FF|LOCKED|129|0.785|0|1`

The RUNNING network policy from v6.20 is unchanged: automatic heartbeat/state traffic remains suppressed while RUNNING; explicit STATUS_REQUEST still works.
