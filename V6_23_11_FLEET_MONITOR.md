# v6.23.11 — fleet CPU0 latency monitor

This revision converts the validated v6.23.10 Stage-B diagnostic into a low-overhead
fleet monitor for scale-up testing.

## What stays enabled

- CPU0 GPTimer sampler: 250 us period (4 kHz)
- latency retention threshold: 50 us
- sampler interrupt level: same configured level as ISR-dispatch esp_timer COMMIT
- runtime wrong-core counting
- late-COMMIT telemetry from the display ISR path
- heartbeat suppression during RUNNING, matching the runs that originally produced
  the ESP02 358/457/800 us stalls

## What is removed/disabled

- task critical-section canary
- same-level ISR canary
- GPIO32 latency-event probe
- analyzer presentation offset (compile-time required to remain 0)
- flash guard diagnostics and ESP-IDF flash counters
- asynchronous post-run diagnostic dump

The countdown scheduler, DS3231 discipline, display publication, synchronization,
brightness, and operator behavior are otherwise unchanged.

## Fleet STATUS extension

FCT2 STATUS remains backward-compatible with the existing 9/10/14/28/31-field
forms. v6.23.11 emits a 42-field form by appending eleven CPU0-monitor fields after
the three RTC continuity counters:

1. `Cpu0MonitorValid` (0/1)
2. `Cpu0MonitorSamples` (hex uint32 on wire)
3. `Cpu0MonitorEventCount` (hex uint32)
4. `Cpu0MonitorWorstUs` (hex uint32)
5. `Cpu0MonitorWorstTask` (<=15 chars)
6. `Cpu0CommitLateCount` (hex uint32)
7. `Cpu0CommitWorstUs` (hex uint32)
8. `Cpu0CommitOverlap` (0/1)
9. `Cpu0WrongCoreCallbacks` (hex uint32)
10. `Cpu0MonitorOverflow` (hex uint32; latency-ring + commit-ring overflow)
11. `Cpu0InterruptLevelMatch` (0/1)

The monitor counters use hexadecimal only on the wire to keep the conservative
worst-case STATUS packet inside the existing 511-byte protocol envelope. The Windows
controller converts them back to ordinary decimal values in `controller_health_*.csv`.

`Cpu0CommitOverlap=1` means at least one retained >=50 us sampler event was
consistent with the timing window of a >=50 us late COMMIT. The overlap window allows
one sampler period of uncertainty before the retained sample deadline; it is a
correlation flag, not proof of exact blocker start time.

## Decision rule

For a future historical-scale pre-entry COMMIT stall >=300 us:

- matching CPU0 monitor event / overlap -> continuous CPU0 blocker remains supported;
- no monitor event while monitor health is valid, wrong-core=0, overflow=0 and level
  match=1 -> one continuous CPU0 interrupt blocker of that duration is ruled out.

Task identity must still be interpreted carefully: a task holding a critical section
can appear as the owner, while a long ISR leaves the interrupted task as a bystander.

## Fleet profiles

`sdkconfig.esp01` through `sdkconfig.esp05` enable the fleet monitor and disable the
old temporary flash/analyzer/canary diagnostics. New ESP06+ profiles cloned for the
15-device fleet should keep the same monitor options:

```text
CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y
CONFIG_FACTORY_CPU0_LATENCY_PERIOD_US=250
CONFIG_FACTORY_CPU0_LATENCY_THRESHOLD_US=50
CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT=y
CONFIG_FACTORY_ANALYZER_PRESENTATION_OFFSET_US=0
# CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS is not set
# CONFIG_SPI_FLASH_ENABLE_COUNTERS is not set
```
