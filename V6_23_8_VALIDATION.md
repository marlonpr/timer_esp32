# v6.23.8 validation performed in this workspace

## Source/static checks

Passed:

- v6.23.2 frame-boundary source contract
- v6.23.3/v6.23.4 health and qualification-guard contracts
- v6.23.5 flash diagnostic invariants
- v6.23.8 deterministic mechanism-control contract: 29/29
- v6.23.1 SQW/core-affinity contract
- v6.23.6 and v6.23.7 runtime positive-control tests are intentionally marked SKIP because v6.23.8 supersedes those induced-NVS runtime modes.

The v6.23.8 analysis tool was also exercised with a synthetic 181-boundary capture containing:

- cache-off probe pulse -500/+500 us at boundary 30 with on-grid COMMIT;
- CPU0 mask pulse -100/+200 us at boundary 60 with +220 us COMMIT residual;
- CPU1 mask pulse -100/+200 us at boundary 90 with on-grid COMMIT.

It reported all three controls PASS and `RESULT=PASS`.

## Not performed here

The container does not contain the user's ESP-IDF v6.0 target toolchain, so the real ESP32 target build must be run on the Windows development machine.

Recommended build:

```powershell
idf.py -B build-esp01-v6238 -D SDKCONFIG=sdkconfig.esp01 build
idf.py -B build-esp02-v6238 -D SDKCONFIG=sdkconfig.esp02 build
```

Only ESP02 has the deterministic injections and GPIO32 probe enabled. ESP01 remains the unstressed control.
