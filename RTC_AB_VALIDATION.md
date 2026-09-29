# DS3231 ON/OFF display-timing A/B validation

This build supports the requested 30-minute A/B without changing the proven
network ARM/START path.

## Analyzer outputs

Presentation-commit diagnostics are enabled in the supplied ESP01-ESP05 profiles.
The GPIO starts low when a countdown is armed, toggles after the visible START
flip/commit, and toggles again after **every one-second display flip**, including
00:00.

- Classic ESP32 (ESP01/ESP02/ESP04): presentation commit = GPIO33
- ESP32-S3 (ESP03/ESP05): presentation commit = GPIO14
- START diagnostic remains GPIO32 on classic ESP32 and GPIO18 on ESP32-S3.

Connect the presentation-commit GPIO from both tested devices to two analyzer
channels. Every transition after START is the corresponding second boundary, so
channel-to-channel edge delta is the software presentation-commit spread at that
second.

## DS3231 ON run

Use the normal profiles, for example:

```powershell
idf.py -B build-esp01-rtc -D SDKCONFIG=sdkconfig.esp01 build flash monitor
idf.py -B build-esp02-rtc -D SDKCONFIG=sdkconfig.esp02 build flash monitor
```

Wait until both devices report `ACQUIRING -> LOCKED`, then run a 30-minute
countdown and capture both GPIO33 channels.

## DS3231 OFF run

Two ready-made baseline profiles are included:

```text
sdkconfig.esp01.no_rtc
sdkconfig.esp02.no_rtc
```

Build them into separate directories so the RTC and non-RTC configurations do
not share CMake state:

```powershell
idf.py -B build-esp01-no-rtc -D SDKCONFIG=sdkconfig.esp01.no_rtc build flash monitor
idf.py -B build-esp02-no-rtc -D SDKCONFIG=sdkconfig.esp02.no_rtc build flash monitor
```

With discipline disabled, the RTC conversion API is identity and no DS3231
capture task is started. START still uses the same raw local deadline and the
display scheduler still follows the same code path, so this is a clean timing A/B.

## What to compare

For every second edge, calculate:

```text
spread_us[k] = edge_device_1[k] - edge_device_2[k]
```

Useful summaries are:

```text
START spread
final spread
max absolute spread
P95 absolute spread
linear slope of spread vs elapsed time
```

The slope is the key long-duration result. With discipline OFF it should reflect
the ESP32-to-ESP32 oscillator-rate difference. With discipline ON it should
collapse toward the DS3231-to-DS3231 rate difference plus presentation jitter.

The diagnostic marks software commit, not emitted light. On classic ESP32 the
actual panel scan can follow the pointer swap by part of a software-scan frame.
