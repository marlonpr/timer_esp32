# v6.23.12 — Fleet monitor telemetry validation

This revision keeps v6.23.11 fleet timing behavior unchanged and closes two rollout-validation gaps.

## Production/fleet behavior
- CPU0 GPTimer monitor remains 4 kHz (250 us), threshold 50 us, same interrupt level as COMMIT.
- FCT2 STATUS remains the same 42-field format introduced in v6.23.11.
- Task and ISR canaries are compile-time options and default OFF.
- GPIO32 latency probe, flash diagnostics, and analyzer presentation offset stay OFF.
- Baseline fleet profiles keep RUNNING heartbeat suppression enabled.

## One-board real telemetry-path validation
Use `sdkconfig.esp01.telemetry_canary`. It enables only the task canary. The canary starts 150 us before display boundary 10 and holds a private CPU0 critical section for 600 us, forcing both a monitor event and a late COMMIT. The ISR canary remains disabled so a clean short run should contain exactly one induced monitor event.

Expected POST_RUN controller CSV values:
- Cpu0MonitorEventCount = 1
- Cpu0MonitorWorstUs approximately 300..600 us
- Cpu0MonitorWorstTask = lat_canary
- Cpu0CommitLateCount = 1
- Cpu0CommitWorstUs approximately 300..600 us
- Cpu0CommitOverlap = 1
- Cpu0WrongCoreCallbacks = 0
- Cpu0MonitorOverflow = 0
- Cpu0InterruptLevelMatch = 1
- StatusFieldCount = 42

Validate the real CSV with:
```powershell
python tools\validate_cpu0_telemetry_canary_csv.py controller_health_....csv --device ESP01
```

After this one validation, return to normal `sdkconfig.esp01` before fleet operation.

## Fleet profiles
`tools/generate_fleet_sdkconfigs.py` generates ESP06..ESP15 directly from `sdkconfig.esp01` and fails if anything except `CONFIG_FACTORY_DEVICE_ID` differs. Generated profiles are already included.

```powershell
python tools\generate_fleet_sdkconfigs.py --check-only
```
