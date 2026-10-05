# v6.23.6 — induced NVS flash positive control

This revision keeps the validated countdown/RTC/display timing path unchanged and adds a qualification-only positive control for the v6.23.5 global flash-window probe.

## Why

A clean run with `FLASH_GUARD_DIAG operations=0` does not by itself prove that the installed guard wrapper intercepts real flash activity. v6.23.6 deliberately causes flash activity and requires the probe to observe it.

## Test mode

On the diagnostic ESP02 profile only:

- `CONFIG_FACTORY_FLASH_GUARD_INDUCED_NVS_TEST=y`
- period = `1,010,100 us` (1.0101 s)
- maximum writes = 120
- namespace/key = `flash_diag/pulse`
- each event changes the value and calls `nvs_commit()`

The 1.0101 s cadence advances write phase by 10.1 ms per one-second display boundary, sweeping the whole second in about 100 writes. ESP01 remains a no-write control.

The esp_timer callback only notifies a normal FreeRTOS task. NVS calls never execute in the countdown ISR or esp_timer callback. No serial logging occurs during the run.

## Required positive-control evidence

After the run, ESP02 must show:

1. `INDUCED_NVS_DIAG ... completed` near 120 for a >=2 minute run;
2. `INDUCED_NVS_EVENT ... guard_operations=N` with N > 0 for every successful write;
3. `FLASH_GUARD_DIAG operations > 0`;
4. one or more retained `FLASH_GUARD_EVENT` records.

If those do not occur, the flash probe is not validated and a natural `operations=0` result is not evidence against flash blocking.

## Mechanism test

Run Analyzer_v8 simultaneously. For each induced write, compare its `scheduled_local_us` / `begin_local_us` with the nearest countdown boundary and compare any physical COMMIT displacement with `callback_lateness_us`.

- write overlaps boundary and COMMIT shifts by roughly the cache-disabled interval: flash blocking mechanism demonstrated;
- guard sees writes but COMMIT does not move: commit path survives these flash windows;
- COMMIT moves while `guard_operations=0`: investigate interrupt masking/critical sections or another source.

This mode intentionally increases NVS wear and is diagnostic only.

## Recommended hardware run

A 3:00 countdown is sufficient; 120 writes finish in about 121.2 s. Keep Analyzer_v8 attached to both COMMIT and REFRESH signals.

At sequence 99, the ideal cadence is `99 * 1.0101 s = 99.9999 s`, only 100 us before the nominal 100-second boundary. Sequences 98 and 100 flank it by about -10.2 ms and +10.0 ms, so the sweep deliberately places writes on both sides of a boundary.

After the run:

```powershell
python tools\analyze_induced_nvs_positive_control.py `
    esp02_serial.txt Analyzer_v8.log --device ESP02
```

Do not use this induced-write build for production or normal qualification.
