# Synchronization quality retry + Wi-Fi diagnostics

The controller performs the synchronization-quality gate. No new command is required on the firmware side.

The firmware change in this revision extends STATUS telemetry to include current station diagnostics:

```text
FCT2|STATUS|<device>|<command>|<state>|<remaining>|<rssi_dbm>|<channel>|<bssid>
```

Example:

```text
FCT2|STATUS|ESP03|0123456789ABCDEF|RUNNING|19|-57|6|AA:BB:CC:DD:EE:FF
```

The values come from `esp_wifi_sta_get_ap_info()`, which is supported by both classic ESP32 and ESP32-S3 in ESP-IDF. The bad-IP rejection, 50 ms blocking UDP receive, deadline-task START_AT scheduler, and STARTED telemetry are unchanged.

Because the STATUS wire format changed from FCT1 to extended FCT2, use the matching updated Windows controller when flashing this firmware. The updated controller is backward-compatible with old FCT1 STATUS packets.
