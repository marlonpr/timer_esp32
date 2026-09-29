# v6.18-A — SQW ring removed from runtime path

Purpose: isolate whether the v6.16/v6.17 raw SQW correlation ring itself causes the observed ESP-side SQW timestamp jitter.

Changes from v6.17:

- `CONFIG_FACTORY_RTC_SQW_CORRELATION_TRACE` is **disabled** in defaults and all RTC device sdkconfigs.
- Therefore the SQW ISR performs no trace-ring `portENTER_CRITICAL_ISR`, no ring index update, and no trace record write.
- Existing 64-sample SQW interval statistics remain enabled (`sqw_period_mean_us`, `sqw_period_rms_us`, `sqw_period_p2p_us`). These are the same lightweight qualification statistics used before the v6.16 trace ring.
- The lwIP early-ingress timestamp hook remains enabled unchanged.
- START and all second-boundary frame publication remain on the proven ISR publication path unchanged.
- GPIO33 commit and GPIO16 refresh markers remain unchanged.
- The production synchronization estimator is unchanged.

`SQW_TRACE` will not be dumped in this build and RTC_QUAL should report `sqw_trace_n=0 sqw_trace_overwrites=0`.

This is A in the planned A/B:

- v6.18-A: no SQW trace ring, ingress hook retained.
- v6.18-B: only if A remains noisy, also remove the ingress hook.
