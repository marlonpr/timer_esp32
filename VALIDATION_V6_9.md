# Validation v6.9

- Base scheduler: v6.6 (no ISR precision-wake change).
- Firmware-host CMake suite: 2/2 passed after STATUS format and gate changes.
- FCT2 STATUS now includes RTC state.
- Firmware START/START_AT guard requires RTC LOCKED when DS3231 discipline is enabled.
- Deferred UDP/5000 near-boundary trace uses a 64-entry queue and priority-4 logger.
- `CONFIG_FACTORY_RX_BOUNDARY_TRACE=y` is explicit in supplied sdkconfig profiles.

A full ESP-IDF target build was not run in this execution environment because ESP-IDF is not installed here.
