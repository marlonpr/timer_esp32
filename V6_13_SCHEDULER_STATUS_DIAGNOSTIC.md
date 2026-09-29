# v6.13 scheduler / STATUS-path diagnostic

This build keeps the v6.12 scheduler unchanged. It does **not** apply the ISR-protected production fix yet.

Added diagnostics:

- `precision_entry_us`: when the display task enters the final precision window.
- `flip_request_us`: immediately before `factory_display_backend_flip()`.
- `flip_commit_us`: immediately after it.
- `COMMIT_DELAY` + `COMMIT_DELAY_TASK` for boundaries whose commit lateness exceeds 100 us.
- Runtime counters are sampled only for cached core-0/unpinned tasks at priority >= 12. The pre-boundary snapshot is skipped if less than 1.5 ms remains, and its cost is logged.
- `STATUS_PATH`: recvfrom return, parse, RX-trace overhead, countdown mutex wait/hold, discipline conversion, STATUS formatting/Wi-Fi diagnostic, sendto entry/return, and handler exit.
- `STATUS_TX`: every STATUS transmission made while RUNNING, including request replies, timer-start telemetry status, one-second state-change status, and heartbeat status.
- RX/STATUS traces are buffered in RAM and dumped after the countdown finishes. The prior live `RX_NEAR_BOUNDARY` UART print has been removed from the measurement window.

The diagnostic specifically separates:

1. late precision-window entry,
2. starvation between wake and flip request,
3. time spent inside `factory_display_backend_flip()`, and
4. STATUS-related work that overlaps the same boundary.

Analyze a device UART log with:

```
python tools/analyze_scheduler_status.py ESP01.log --controller-csv controller_tx.csv
```
