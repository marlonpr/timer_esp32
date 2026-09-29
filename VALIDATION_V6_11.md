# v6.11 validation

- Based on v6.9 baseline scheduler (no ISR precision wake).
- Preserves RTC LOCK gate, RX_NEAR_BOUNDARY trace, GPIO33 non-atomic marker order, configured factory LAN policy, and current Wi-Fi configuration.
- Enables FreeRTOS trace facility and run-time stats using ESP Timer.
- Task inventory is collected once per armed run before START timing.
- Runtime snapshots are collected only after GPIO33 marker transitions.
- Marker gaps over 100 us emit MARKER_GAP plus ranked MARKER_GAP_TASK lines.
- Existing firmware-host suite: 2/2 passed.
- Full idf.py firmware build was not run in this environment because ESP-IDF is unavailable here.
