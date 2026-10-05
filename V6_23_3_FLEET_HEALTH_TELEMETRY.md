# v6.23.3 — Fleet health telemetry

v6.23.3 is an instrumentation-only follow-up to v6.23.2. Countdown scheduling, DS3231 discipline, SQW core-1 handling, pooled synchronization inputs, and refresh-boundary framebuffer adoption are unchanged.

## STATUS extension

The firmware emits a 28-field FCT2 STATUS that keeps the established first 14 fields and appends:

1. local ESP timer rate versus RTC (`rate_ppm` diagnostic);
2. RTC fit-outlier count;
3. RTC temperature;
4. SQW ISR core;
5. health-validity flags;
6. selected forward-sync source offset at its sample epoch;
7. sync epoch in raw local microseconds;
8. the same epoch mapped into the disciplined timebase;
9. `Master - Disciplined` at that same epoch;
10. START error;
11. scheduler lateness;
12. START display-publish lateness;
13. worst display-publish lateness for the run;
14. `frame_not_ready` count.

Health flag bits are:

```text
bit 0 (1): synchronized epoch fields valid
bit 1 (2): START scheduler fields valid for this CommandId
bit 2 (4): display publication fields valid for this CommandId
```

## DS3231 qualification field

At the selected forward-sync epoch:

```text
master_minus_disciplined =
    source_offset_us + offset_epoch_local_us - offset_epoch_disciplined_us
```

This is the quantity whose slope between two warm STARTs measures the countdown/RTC-disciplined timebase relative to the common controller. The raw `master_minus_local` offset is deliberately not used for RTC qualification because it tracks the ESP32 `esp_timer` oscillator.

## Compatibility

The controller parser continues to accept legacy 9-, 10-, and 14-field FCT2 STATUS packets. Firmware v6.23.3 requires the matching controller update for the complete 28-field STATUS datagram because its maximum protocol packet length is increased to 383 bytes.
