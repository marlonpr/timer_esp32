# v6.23.1 SQW core-affinity correction

The first v6.23 hardware test showed `sqw_core=0` on both ESP01 and ESP02.

Root cause: `gpio_config()` was called with `GPIO_INTR_POSEDGE` before the pinned
core-1 installer. ESP-IDF enables the interrupt during `gpio_config()` and records
the current CPU as the GPIO ISR core. `rtc_discipline_init()` runs on core 0, so
the later installer could not move the already-selected GPIO interrupt core.

v6.23.1 changes initialization order only:

1. Configure SQW input with `GPIO_INTR_DISABLE`.
2. Run the ISR-service installer pinned to core 1.
3. Assert the installer really runs on core 1.
4. Install the GPIO ISR service.
5. Set SQW to `GPIO_INTR_POSEDGE`.
6. Attach the per-pin handler; it is enabled on the service-owning CPU.

The v6.23 one-sided late-only fit filter is unchanged. Synchronization protocol and
controller behavior are unchanged.

Hardware acceptance: `RTC_QUAL ... sqw_core=1` on both devices.
