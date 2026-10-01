# v6.20 production-silent RUNNING

This release promotes the validated v6.19-C network-silent behavior to the production policy.

## RUNNING network policy

- Per-second unsolicited `STATE_CHANGE`: disabled.
- Periodic unsolicited RUNNING `HEARTBEAT`: disabled before Wi-Fi/RTC/status formatting/send work.
- Explicit `STATUS_REQUEST`: fully supported at all times, including RUNNING.
- UDP command reception remains active; RESET remains available during a countdown.
- STARTED and FINISHED/state-transition telemetry remain enabled.

The controller is expected to stop automatic STATUS polling while any production countdown is RUNNING. An operator may request a one-shot unicast STATUS from the UI when needed.

The timing/RTC implementation is otherwise unchanged from the validated v6.19-C run: DS3231 discipline, SQW residual filtering, ISR publication, GPIO33/GPIO16 diagnostics, RTC qualification logging, and the asynchronous post-run diagnostic dump remain intact in this validation-capable source tree.

`NETWORK_SILENT_DIAG running_heartbeat_suppressed=N` is retained as an observability counter. It has no network side effect.
