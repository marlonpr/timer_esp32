# v6.23.1 SQW robustness hardware test

Firmware only; use the existing v6.22.3 Windows controller.

## Immediate acceptance
After boot, verify:

- `SQW ISR service installed: installer_core=1 requested_core=1`
- `Started: ... isr_core=1 fit_filter=late_only core_affinity_fix=1`
- after SQW edges: `RTC_QUAL ... sqw_core=1 ... queue_drops=0` on ESP01 and ESP02.

If either device reports `sqw_core=0`, stop; core migration did not take effect.

## Short run
1. Analyzer v8 active.
2. Normal 20-60 s countdown.
3. Confirm `frame_not_ready=0`, `queue_drops=0`, RTC stays LOCKED.
4. Compare GPIO16 COMMIT->REFRESH timing with v6.22 baseline.

## Network stress
Repeat several STARTs with the external ESP32 load generator at 10 Mbps and then the previously demonstrated ~17 Mbps effective load.

Watch:
- `sqw_core=1` throughout.
- `fit_last_outlier_us` for rejected fit points remains non-negative.
- SQW fit RMS/rate do not show the old network-correlated capture shift.
- Analyzer physical SQW remains unchanged.
- `frame_not_ready=0`; GPIO16 timing does not regress.

## Long run
If short stress passes, run 600 s and compare RTC rate stability and display-vs-physical-SQW trend against v6.22/v6.20.
