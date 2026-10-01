# v6.19 A/B validation checklist

## Build

Example ESP01 A build:

```powershell
idf.py -B build-esp01-v619a -D SDKCONFIG=sdkconfig.esp01 build
```

Verify generated config contains:

```text
CONFIG_FACTORY_RTC_FIT_RESIDUAL_REJECTION=y
CONFIG_FACTORY_RTC_FIT_RESIDUAL_THRESHOLD_US=4
CONFIG_FACTORY_POST_RUN_TIMING_DUMP=y
```

A must also contain:

```text
CONFIG_FACTORY_PER_SECOND_STATE_CHANGE_STATUS=y
```

B must contain:

```text
# CONFIG_FACTORY_PER_SECOND_STATE_CHANGE_STATUS is not set
```

## Runtime acceptance

- START remains `scheduler_lateness_us=0`.
- `frame_not_ready=0`.
- Quiet 64-edge SQW window: p2p <= 2 us.
- Isolated loaded timestamp tails increment `fit_outliers` instead of increasing
  the fit residual/slope leverage. `rejected` should remain reserved for physical
  edge/glitch rejection.
- Post-run dump prints `POST_RUN_DIAG_BEGIN` / `POST_RUN_DIAG_END`; no TWDT event
  should occur and UDP status requests should still be serviced while it prints.
- ISR trace header reports first/worst retention and any millisecond tail should
  appear in a `retention=WORST` record.
- In B, there must be no per-second `STATUS_TX reason=STATE_CHANGE` while RUNNING.
