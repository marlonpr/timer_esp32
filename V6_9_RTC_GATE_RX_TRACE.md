# v6.9 RTC lock gate + control-port arrival trace

Base: v6.6 DS3231 qualification build. The v6.8 ISR precision-wake scheduler is intentionally **not** included, so this package remains the unprotected scheduler baseline.

Changes:

- FCT2 STATUS now appends RTC discipline state (`LOCKED`, `ACQUIRING`, `HOLDOVER`, `UNINITIALIZED`, `DISABLED`).
- When DS3231 discipline is enabled, firmware rejects START and START_AT unless discipline is currently LOCKED. The ACK remains `NOT_SYNCED`; UART identifies the RTC state.
- `RX_NEAR_BOUNDARY` records UDP/5000 traffic that arrives within 15 ms before a running countdown boundary.
- Receive timestamping happens immediately after `recvfrom`; logging is deferred through a 64-entry queue to a priority-4 logger, so UART output is not executed in the precision window.
- The trace is application-level and does not see Wi-Fi management traffic or packets on unrelated UDP ports.
