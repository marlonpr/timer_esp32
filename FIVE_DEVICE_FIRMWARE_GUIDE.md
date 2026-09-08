# Five-device firmware setup

Use the same ESP-IDF source for all five timer devices. The only required per-device difference is the exact protocol identity:

- ESP01
- ESP02
- ESP03
- ESP04
- ESP05

Wi-Fi credentials may be the same or different depending on which AP/repeater each unit should join.

Use a separate sdkconfig and build directory for every unit. Example for ESP03:

```powershell
idf.py -B build-esp03 -D SDKCONFIG=sdkconfig.esp03 -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp03" set-target esp32
idf.py -B build-esp03 menuconfig
idf.py -B build-esp03 build
idf.py -B build-esp03 -p COMx flash monitor
```

Repeat with `esp01` through `esp05`. Do not reuse the same build directory across device profiles because CMake caches the SDKCONFIG path.

For the first 5-device performance baseline, leave the controller SYNC path-delay mode at **NONE (0/0 ms)**.
