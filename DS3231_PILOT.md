# Factory Timer DS3231 pilot

This build integrates the DS3231 1 Hz discipline package into the existing dual-target
Factory Timer firmware without changing the proven network synchronization / START path.

## Wiring

The RTC wiring is fixed for this pilot:

| Signal | ESP32 / ESP32-S3 GPIO |
|---|---:|
| DS3231 SDA | 21 |
| DS3231 SCL | 22 |
| DS3231 INT/SQW | 27 |
| GND | GND |
| VCC | 3.3 V (recommended for ESP32 logic compatibility) |

The DS3231 INT/SQW pin is configured for a 1 Hz square wave. The firmware enables the
ESP32 internal pull-up for GPIO27.

## Diagnostic pin changes

GPIO21/22 can no longer be used by timing diagnostics because they belong to I2C.
The active profiles therefore use:

| Target | START diagnostic |
|---|---:|
| Classic ESP32 (ESP01/ESP02/ESP04) | GPIO32 |
| ESP32-S3 (ESP03/ESP05) | GPIO18 |

The optional display-commit diagnostic remains disabled by default. If enabled, its new
default is GPIO33 on classic ESP32 and GPIO14 on ESP32-S3.

## Timing behavior

START remains exactly on the existing raw `esp_timer` deadline:

1. Network synchronize devices.
2. Apply the existing gate.
3. Freeze `T*`.
4. Use the existing high-priority deadline task for `Armed -> Running`.
5. Raise the START diagnostic edge at that transition.

At `Armed -> Running`, the timer records a disciplined timestamp. From then on,
`remaining_seconds` is recomputed from absolute disciplined elapsed time rather than
raw ESP32 elapsed time.

The display also keeps the original raw START deadline. The high-priority TimerTask
publishes the exact disciplined `Armed -> Running` epoch to the display scheduler without
moving the first visible frame. Every later one-second boundary is derived from that same
disciplined epoch and converted back to a fresh local `esp_timer` deadline. The conversion
is re-evaluated after coarse sleeps so continuous rate updates can affect the next visible
boundary.

## RTC estimator

- ACQUIRING: 33 valid edges (32 seconds of span)
- Fit window: up to 129 points (128 seconds of span)
- SQW-loss timeout: 3.5 seconds
- LOCKED -> HOLDOVER preserves the last fitted rate
- SQW return forces fresh acquisition while retaining the last good rate during reacquisition
- Rate installation re-anchors the mapping so disciplined time does not step
- the disciplined anchor is retained internally in nanoseconds so repeated 1 Hz
  re-anchors do not discard the same fractional microsecond every second
- DS3231 OSF is cleared only after sane SQW edges are observed
- DS3231 temperature is sampled periodically

If no SQW edge is seen after startup, the estimator enters HOLDOVER after 3.5 seconds and
logs the condition. If I2C/RTC initialization itself fails, the firmware logs an error;
the discipline conversion API then behaves as identity mapping so the existing raw timer
behavior remains available for diagnosis.

## Expected serial log landmarks

Typical startup / acquisition messages include:

```text
DS3231 initialized on SDA=21, SCL=22 at 100000 Hz
INT/SQW configured for 1 Hz output
Started: SQW GPIO=27 acquire_points=33 fit_points=129 holdover_ms=3500
DS3231 discipline active: SDA=GPIO21 SCL=GPIO22 SQW=GPIO27 acquisition=32s fit_window=128s
State ACQUIRING -> LOCKED
RTC rate locked: ... rate_ppm=... rms_us=... temp=...C
```

A running START log now also includes `disciplined_us=`.

## Host validation

The firmware-host suite contains an additional dual-clock test proving that:

- START is still controlled by raw local time.
- A raw-local second crossing does not decrement the countdown unless the disciplined
  clock has crossed its absolute one-second boundary.
- Countdown finish occurs from disciplined elapsed time.

A full ESP-IDF target build still needs to be run in an ESP-IDF 6.x environment.

## Qualification instrumentation

This revision adds low-priority machine-readable `RTC_QUAL` samples plus `RTC_SYNC_QUAL`
and `RTC_START_QUAL` records. They preserve the exact synchronization offset associated
with each START so two STARTs separated by a long interval can recover RTC-to-master rate
and, across devices, RTC-to-RTC rate with the PC frequency error cancelled.

Do not interpret `rate_ppm` by itself as DS3231 error. The estimator reports the ESP32
clock rate relative to its local RTC. See `RTC_QUALIFICATION.md` and
`tools/rtc_qualification.py`.
