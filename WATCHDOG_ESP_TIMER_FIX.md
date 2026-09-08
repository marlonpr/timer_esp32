# Watchdog-safe START_AT scheduler

The original timing proof used a 2 ms UDP receive timeout and repeatedly called `esp_timer_get_time()` from `udp_command`. With `CONFIG_FREERTOS_HZ=100`, that loop could remain runnable aggressively enough to starve the idle task and trigger the task watchdog.

The validated replacement is the deadline-task scheduler described in `DEADLINE_TASK_START_SCHEDULER.md`:

- `udp_command` blocks in `recvfrom()` with a 50 ms timeout. Incoming UDP packets still wake it immediately.
- Accepted START/START_AT commands notify a dedicated `countdown_timer` task.
- That task sleeps for whole RTOS ticks until shortly before the absolute local target.
- Only the final short window (roughly <= one 10 ms tick + 2 ms at 100 Hz) uses `esp_timer_get_time()` in a precision loop.
- The task records the exact local transition timestamp, then queues STARTED telemetry back to the UDP task.
- Normal operation no longer continuously busy-polls and has been observed running for many minutes without the previous `IDLE0 / udp_command` watchdog failure.

Earlier experimental esp_timer-callback/GPTimer branches are not used in this build.

The next experiment in this package is controlled synchronization-path delay injection. It changes only SYNC calibration timing; the watchdog-safe START scheduler remains unchanged.
