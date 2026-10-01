> Historical validation note: v6.19-C established the behavior now adopted by v6.20 production-silent RUNNING.

# v6.19-C network-silent RUNNING diagnostic

This variant is derived directly from v6.19-B. It changes only unsolicited application-layer status traffic while the countdown is RUNNING.

- Per-second RUNNING `STATE_CHANGE`: disabled, unchanged from B.
- Periodic unsolicited RUNNING `HEARTBEAT`: disabled by `CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT=y`.
- Suppression occurs before Wi-Fi diagnostics, RTC reads, status formatting, and `sendto()`.
- Explicit `STATUS_REQUEST` replies remain fully functional.
- UDP command reception remains active; RESET remains available during the run.
- `TIMER_STARTED`, `FINISHED`, RTC discipline, SQW residual filtering, ISR publication, GPIO33/GPIO16 diagnostics, RTC_QUAL logging, and the asynchronous post-run dump are unchanged.
- Heartbeat cadence is advanced even when a heartbeat is suppressed, avoiding a continuously-overdue branch.
- The counter `NETWORK_SILENT_DIAG running_heartbeat_suppressed=N` is printed by the post-run diagnostic task.

For the C comparison, use the matching controller variant so it sends no STATUS sweep and no periodic STATUS requests during the measured RUNNING interval.
