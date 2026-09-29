# DS3231 relative-rate qualification

This pilot qualifies **agreement between DS3231 modules**, not absolute error versus the PC.
The Windows controller/master uses QPC as its timebase, so a common PC frequency error must
not be interpreted as DS3231 error.

## Sign convention

The firmware receives and stores the synchronization offset as:

```text
O = master - local
```

Let:

```text
E = ESP32 local-clock rate error
D = DS3231 rate error
M = PC/master rate error
```

The DS3231 discipline estimator reports:

```text
r = E - D
```

For two synchronized STARTs separated by a long interval, the change in the **applied**
`master-local` offset is evaluated at the two SYNC_SET epochs that produced those STARTs:

```text
q = E - M = -ΔO / ΔT_sync
```

`RTC_START_QUAL` copies each START's exact `sync_local_us` and
`sync_estimated_master_us`, so the analyzer averages `r` over that same sync-to-sync
interval. This avoids bias if the two sync-to-START ages differ.

Therefore each module's RTC rate relative to the common PC master is:

```text
D - M = q - r
```

The quantity used to compare two RTCs is:

```text
(D1 - M) - (D2 - M) = D1 - D2
```

The master rate error cancels. For a larger batch, compare every `D-M` value with the
**batch median**, not with zero.

## Firmware records

Qualification logging is enabled by default in this pilot with:

```text
CONFIG_FACTORY_RTC_QUAL_LOGS=y
CONFIG_FACTORY_RTC_QUAL_LOG_INTERVAL_S=16
```

The logger runs as a low-priority task so it does not replace or modify the high-priority
START deadline path. While qualification logging is enabled, the older duplicate
`RTC rate locked` line is suppressed; `RTC_QUAL` carries the same estimator data with
higher precision and more health fields.

### `RTC_QUAL`

Emitted every 16 seconds by default:

```text
RTC_QUAL device=ESP01 local_us=... disciplined_us=... state=LOCKED rate_ppm=-6.842000 rms_us=3.500 points=80 accepted=... rejected=... inferred_missing=... queue_drops=... temp_valid=1 temp_c=25.25
```

For qualification, use only samples with `state=LOCKED`.

`rate_ppm` is `r = E-D`. It is **not** the absolute DS3231 error.

### `RTC_SYNC_QUAL`

Emitted when a `SYNC_SET` is applied:

```text
RTC_SYNC_QUAL device=ESP01 sync=... local_us=... estimated_master_us=... offset_master_minus_local_us=... best_rtt_us=... state=LOCKED rate_ppm=... rms_us=... points=... temp_valid=1 temp_c=...
```

The important fields are the applied local timestamp, the estimated master timestamp at
that same SYNC_SET epoch, and the exact `master_minus_local` offset selected by the
controller. The offline calculation uses these sync timestamps directly because the offset
measurement belongs to the sync epoch, not to the later START deadline.

### `RTC_START_QUAL`

Emitted after the real high-priority `Armed -> Running` transition:

```text
RTC_START_QUAL device=ESP01 command=... sync=... sync_local_us=... sync_estimated_master_us=... offset_master_minus_local_us=... best_rtt_us=... target_master_us=... actual_local_us=... estimated_master_us=... start_error_us=... scheduler_lateness_us=... sync_age_local_us=... disciplined_start_us=...
```

The START record copies the exact `SYNC_SET` metadata used to calculate that START. This
means a saved serial log remains self-contained even if many later syncs occur.

## Two-module pilot procedure

1. Flash the same qualification build to both devices.
2. Connect one DS3231 to each device.
3. Wait until both log `State ACQUIRING -> LOCKED`.
4. Start serial capture for each device and leave it running.
5. Run the normal controller START sequence once. The controller should perform its normal
   full synchronization before START.
6. Leave both devices powered and locked for about 30 minutes.
7. Run the normal controller START sequence a second time, again with a fresh synchronization.
8. Save one serial log file per device.
9. Run:

```powershell
python tools\rtc_qualification.py ESP01.log ESP02.log --duration 5999
```

`5999` seconds corresponds to `99:59`.

The tool uses the first and last `RTC_START_QUAL` records by default. If a log contains
extra STARTs, select the desired pair with `--start-a` and `--start-b`.

Example:

```powershell
python tools\rtc_qualification.py ESP01.log ESP02.log --start-a 1 --start-b 2 --duration 5999
```

## Analyzer output

For each device the analyzer reports:

```text
q = E-M
mean r = E-D
D-M = q-r
```

For two logs it also reports:

```text
D1-D2
predicted separation at the selected countdown duration
```

For three or more logs it reports each module's deviation from the batch median and the
batch max-min RTC rate spread.

Because ppm multiplied by seconds gives microseconds directly:

```text
0.5 ppm * 5999 s = 2999.5 us ~= 3.0 ms
```


## Re-anchor precision fix

The first pilot re-anchored disciplined time to an integer-microsecond value after every
accepted SQW edge. A stable fractional microsecond could therefore be discarded once per
second and accumulate as an artificial 0..1 ppm rate error. The corrected implementation
keeps the internal disciplined anchor in nanoseconds and rounds only below one nanosecond;
the external API remains in microseconds.

A deterministic regression using the ESP02-like 4.245 ppm case reproduces the old error:

```text
legacy: -4530 us over 6000 s = -0.755 ppm
fixed:       0 ns over 6000 s in the same simulation
```

Run the included regression with:

```powershell
python tools\rtc_anchor_precision_test.py
```

## Thermal characterization

`RTC_QUAL` is also useful without any STARTs. Leave a device logging from cold startup
through panel warm-up and inspect:

```text
local_us, rate_ppm, rms_us, temp_c
```

When the DS3231 is a stable TCXO, most slow movement in `rate_ppm = E-D` is the ESP32
local oscillator moving relative to it. The DS3231 temperature register measures the RTC
package, not the ESP32 crystal, so thermal correlation is better when the RTC module is
mounted close to the ESP32 oscillator/board area.

A recurring RMS disturbance on approximately a 64-second cadence is consistent with the
DS3231 temperature-conversion / TCXO compensation cycle; asynchronous disturbances that
track network activity are more consistent with GPIO ISR timestamp latency.

## GPIO used by this package

The validated ESP01 pilot log used:

```text
SDA     GPIO21
SCL     GPIO22
INT/SQW GPIO27
```

The active ESP01-ESP05 profiles in this package therefore use GPIO27 for INT/SQW. The
Kconfig value remains configurable through `FACTORY_DS3231_SQW_GPIO` if hardware requires
another free GPIO.
