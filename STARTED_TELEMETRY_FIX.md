# STARTED telemetry fix

The deadline-task scheduler itself was validated by hardware logs: the countdown transitioned from ARMED to RUNNING at the exact local target with `scheduler_lateness_us=0`.

The Windows controller still showed `Waiting for STARTED telemetry...` because periodic `STATUS_REQUEST` packets were accidentally treated as scheduler-mutating accepted commands. `ProcessCommand()` intentionally returns `AckResult::Accepted` for STATUS_REQUEST with `send_ack=false`; the firmware's generic accepted-command branch then cleared `scheduled_start_metadata`. The deadline task still had the countdown target and started correctly, but at start time the peer/target/offset metadata was gone, producing `estimated_master_us=-1` and suppressing the FCT2 STARTED packet.

Fix: only RESET clears scheduled start metadata. STATUS_REQUEST is now strictly non-mutating.

Expected start log after the fix:

```
Countdown started: ... estimated_master_us=<target +/- scheduler lateness> scheduler_lateness_us=...
```

The controller should populate `Actual start Master` and `Start timing error` instead of remaining at `Waiting for STARTED telemetry...`.
