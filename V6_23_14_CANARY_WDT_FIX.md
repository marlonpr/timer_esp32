# v6.23.14 telemetry-canary watchdog fix

This revision changes only the validation-only task-canary wait loop.

## Root cause

The classic ESP32 fleet profiles use `CONFIG_FREERTOS_HZ=100`.
`pdMS_TO_TICKS(1)` therefore evaluates to zero.  The telemetry-canary task is
created at the highest FreeRTOS priority, so `vTaskDelay(0)` merely yielded to
equal/higher-priority work and left `lat_canary` continuously READY.  IDLE0
could not run and the Task Watchdog fired every five seconds before boundary 10.

## Fix

The pre-boundary polling wait is now `vTaskDelay(1)`, meaning exactly one RTOS
tick regardless of tick frequency.  The canary remains a validation-only build
option and is still disabled in all fleet profiles.

No production/fleet timing, monitor sampling, STATUS protocol, controller
telemetry, or sdkconfig identity settings are changed.
