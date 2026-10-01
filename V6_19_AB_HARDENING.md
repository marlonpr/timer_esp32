# v6.19 A/B hardening build

This build follows the successful two-device 30-minute DS3231 qualification.
The START control path is unchanged.

## Common changes in A and B

1. **Post-run UART diagnostics no longer run in `CommandTask`.**
   A dedicated CPU0 task at priority 1 performs the diagnostic dump, with a
   one-RTOS-tick pause after each buffered record. `CommandTask` returns to
   `recvfrom()` immediately after requesting the dump.

2. **RTC fit-only timestamp residual rejection.**
   A physical SQW edge remains accepted for liveness and sequence accounting,
   but the shared center timestamp is excluded from the rate regression when two
   consecutive one-second intervals have opposite-sign residuals of at least
   4 us against the current locked rate. New `RTC_QUAL` fields are:
   `fit_outliers`, `fit_last_outlier_us`, and `fit_max_outlier_us`.

3. **ISR publish trace retains useful tails.**
   Instead of only the first 64 events, the trace keeps the first 8 boundaries
   plus the worst 8 later boundaries ranked by callback-entry lateness. This is
   sufficient to capture a future millisecond-scale callback tail together with
   its arm margin and marker timestamps.

4. SQW correlation ring remains disabled. Early lwIP ingress tracing remains
   enabled. No IRAM hardening is included in this A/B.

## A vs B

- **v6.19-A:** per-second unsolicited `STATE_CHANGE` remains enabled.
- **v6.19-B:** per-second RUNNING `STATE_CHANGE` is disabled. STATUS request
  replies remain; a 2-second heartbeat remains available; STARTED/FINISHED
  telemetry remains.

## Suggested run

Use Analyzer v8 with the same six wires. Keep brightness/content fixed.
Run A and B for 5-10 minutes each after a short post-flash settle. Compare:

- GPIO33 detrended residual RMS/P95/P99/max and worst callback-lateness records
- GPIO16 adoption distribution
- `fit_outliers` count and maximum residual
- internal SQW p2p/RMS
- `STATUS_TX reason=STATE_CHANGE` count
- watchdog events (expected: zero)

The long-term DS3231 drift question is already answered by the 30-minute run;
this A/B is specifically for traffic-induced latency tails.
