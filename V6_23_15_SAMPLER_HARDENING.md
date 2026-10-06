# v6.23.15 — CPU0 sampler and rollout hardening

This revision is the pre-rollout correction to v6.23.14. Normal fleet behavior is
unchanged except for the CPU0 monitor implementation and a stricter RTC START gate.

## Sampler correction

The 4 kHz GPTimer is now a free-running 1 MHz counter with auto-reload disabled.
Every callback receives the hardware `alarm_value` that first became overdue and
hardware `count_value` on ISR entry. Authoritative lateness is therefore:

```
lateness_us = count_value - alarm_value
```

The next alarm is the first absolute 250 us grid deadline after all deadlines already
overdue. `missed_periods = floor(lateness_us / 250)` counts additional grid deadlines
swallowed by the blocker. This removes the v6.23.14 one-period ambiguity.

The alarm rearm configuration is stored in DRAM and `gptimer_set_alarm_action()` is
used from ISR context with GPTimer handler/control/cache-safe options enabled. The
callback is IRAM and retained state is DRAM. Task names/priorities remain resolved
post-run; the ISR stores only the raw task handle.

## Positive controls before rollout

Three ESP01 validation profiles are included:

- `sdkconfig.esp01.telemetry_canary`: legacy 600 us task critical section across b10;
  proves monitor -> late COMMIT -> overlap -> STATUS -> CSV.
- `sdkconfig.esp01.sampler_task_canary`: 1000 us task critical section at +10.5 s,
  halfway between COMMIT boundaries. Correct monitor result must be >=750 us and
  report at least three missed periods, with no late COMMIT.
- `sdkconfig.esp01.isr_canary`: 1000 us same-level esp_timer ISR at +16.5 s. Correct
  monitor result must be >=750 us and report at least three missed periods. The task
  name is the underlying task interrupted by the ISR; it is contextual, not culprit.

`sdkconfig.esp01.monitor_off` is provided for a matched COMMIT-jitter A/B run.

## Cache safety

Fleet profiles require:

```
CONFIG_GPTIMER_ISR_CACHE_SAFE=y
CONFIG_GPTIMER_OBJ_CACHE_SAFE=y
CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM=y
CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM=y
```

This keeps the sampler responsive during flash-cache-disabled windows rather than
misclassifying a flash operation as CPU0 interrupt masking.

## RTC START gate

START_AT now requires the full 129-point RTC fit, not 64 points, while preserving the
existing `RtcFitRmsUs <= 3.0`, LOCKED, zero queue-drop and valid-temperature guards.
This prevents the fleet from starting during the early warm-up curvature seen on
ESP01.

## Fleet profiles

All `sdkconfig.esp01` ... `sdkconfig.esp15` profiles are classic ESP32 identity-only
clones of ESP01. DHCP remains enabled and station hostname comes from
`CONFIG_FACTORY_DEVICE_ID`. Canary profiles are validation-only and must never be
used for fleet exposure.
