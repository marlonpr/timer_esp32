# Deadline-task START_AT scheduler

The START_AT scheduler no longer relies on ESP Timer callbacks or GPTimer alarms.

The dedicated `countdown_timer` task sleeps until close to the requested local deadline using a FreeRTOS task notification timeout. It then uses `esp_timer_get_time()` only during a short final precision window. At the default 100 Hz FreeRTOS tick rate, that final window is bounded to about one tick plus 2 ms (roughly <= 12 ms).

This is materially different from the old watchdog-triggering design: the UDP task no longer polls every 2 ms for the entire lifetime of the firmware. `udp_command` blocks in `recvfrom()` with a 50 ms timeout, while the deadline task is normally blocked and only becomes CPU-active for a few milliseconds immediately before START_AT.

The START_AT transition remains local and independent of packet arrival once the absolute target is received. STARTED telemetry carries the timestamp captured at the actual local transition.

Expected logs:

    High-resolution countdown scheduler ready; source=deadline_task ...
    Countdown scheduler armed: source=deadline_task ...
    Countdown started: ... scheduler_lateness_us=...
    Countdown second: 19
    ...
    Countdown finished: ...
