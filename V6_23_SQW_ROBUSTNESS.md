# v6.23 SQW robustness

This is a firmware-only change based on v6.22. The Windows controller remains v6.22.3.

## 1. SQW ISR moved away from Wi-Fi/core 0

The GPIO ISR service is installed by a one-shot FreeRTOS task pinned to CPU core 1. This moves SQW timestamp capture away from the Wi-Fi/control workload on core 0.

This build deliberately does **not** add `ESP_INTR_FLAG_IRAM`: the existing SQW ISR posts to a FreeRTOS queue, while this ESP-IDF v6 configuration leaves `CONFIG_FREERTOS_IN_IRAM` disabled. Making the interrupt cache-disabled/IRAM-safe would therefore require a separate queue/mailbox redesign or moving the required FreeRTOS ISR path into IRAM. v6.23 isolates the two measured issues: core affinity and one-sided fit filtering.

Production diagnostics must therefore report:

```
sqw_core=1
```

The firmware treats an already-installed GPIO ISR service as an initialization error, rather than silently losing the intended core-1 affinity. In this project the RTC discipline component is the only installer of the GPIO ISR service.

## 2. One-sided fit-only ISR-latency filter

A physically accepted SQW edge remains valid for liveness and sequence accounting. Only its ESP-side timestamp can be omitted from the rate regression.

For three consecutive timestamps A, B, C and expected one-second interval P:

```
r1 = (B-A) - P
r2 = (C-B) - P
```

B is excluded from the fit only when:

```
r1 >= +threshold
r2 <= -threshold
```

with the existing threshold (default 4 us).

This is the signature of an isolated **late** center timestamp. The opposite signature (`r1 <= -threshold`, `r2 >= +threshold`) is not rejected, because ISR latency cannot timestamp the physical SQW edge early. Keeping those apparently early centers prevents the residual gate from locking onto a fit that was pulled late by neighboring delayed ISR timestamps.

Gross validity remains separate and unchanged: glitch rejection, missing-edge inference, sane-rate limits, holdover, queue-drop qualification and RTC fit RMS/point-count qualification remain active.

## Validation target

Repeat the same network-activity stress that previously produced SQW timestamp contamination while keeping Analyzer v8 active.

Required checks:

1. Every `RTC_QUAL` line reports `sqw_core=1`.
2. `queue_drops=0` and RTC remains `LOCKED`.
3. `fit_last_outlier_us` for rejected fit timestamps is never negative; apparently early signatures are retained.
4. Compare silent-RUNNING SQW timestamp repeatability with the v6.20 baseline (~1% contaminated edges).
5. GPIO16 COMMIT->REFRESH and pair-gap statistics must not regress; existing 12 ms acceptance remains in force.
6. START synchronization behavior must remain unchanged from v6.22/v6.22.3.

A 600 s run is preferred after a short 60 s smoke test.
