# v6.23.15a mid-second canary compile fix

The v6.23.15 `sdkconfig.esp01.sampler_task_canary` profile enables
`FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND`. In that mode Kconfig intentionally
hides the boundary-only symbols `FACTORY_CPU0_LATENCY_CANARY_BOUNDARY` and
`FACTORY_CPU0_LATENCY_CANARY_LEAD_US`.

The C++ source still referenced both macros unconditionally whenever the task
canary was enabled, causing the target compile failure. The source now selects
constants by canary mode:

- mid-second: boundary=0, lead=0, uses AFTER_RUN_US and HOLD_US;
- boundary canary: uses BOUNDARY, LEAD_US and HOLD_US as before.

This is a compile-only correction. The 1000 us mid-second canary, 4 kHz sampler,
STATUS protocol, controller, and normal fleet profiles are unchanged.
