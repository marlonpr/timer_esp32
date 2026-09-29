# DS3231 qualification instrumentation validation

Validation performed on the v6.6 qualification source package:

- Existing firmware-host CMake build: PASS.
- `factory_timer_core_debug_0`: PASS.
- `factory_timer_core_debug_1`: PASS.
- Existing disciplined countdown behavior test remains PASS.
- `tools/rtc_qualification.py` Python syntax compilation: PASS.
- Synthetic two-device analyzer check: PASS.
  - Synthetic ESP01: `q ~= -5 ppm`, `r = -6 ppm`, `D-M ~= +1 ppm`.
  - Synthetic ESP02: `q ~= +2 ppm`, `r = +0.5 ppm`, `D-M ~= +1.5 ppm`.
  - Analyzer recovered `D1-D2 ~= -0.5 ppm` and about `-3.0 ms` at 5999 s.
- Active ESP01-ESP05 configurations use DS3231 INT/SQW GPIO27, matching the supplied
  successful ESP01 hardware log.
- SDA remains GPIO21 and SCL remains GPIO22.
- START diagnostic remains GPIO32 on classic ESP32 and GPIO18 on ESP32-S3.

The qualification additions are diagnostic-only. They do not alter the existing network
sync sequence, `T*`, high-priority raw-local START deadline, or disciplined post-START
countdown calculation.

A complete `idf.py build` still requires an ESP-IDF 6.x installation and is not claimed
from this container environment.

Additional v6.6 timing fixes:

- Reproduced the legacy per-SQW re-anchor bias at the ESP02-like +4.245 ppm rate:
  `-4530 us / 6000 s = -0.755 ppm`.
- Nanosecond-anchor implementation: `0 ns` error in the same deterministic 6000 s
  regression (`tools/rtc_anchor_precision_test.py`).
- `rtc_discipline.c`: strict C17 `-Wall -Wextra -Werror` syntax check PASS.
- `factory_display.cpp`: strict C++17 `-Wall -Wextra -Werror` syntax check PASS.
- Firmware-host CMake tests after the fixes: 2/2 PASS.
- Presentation diagnostic now toggles on START and every later one-second flip.
- Final <=2 ms display spin no longer enters the RTC critical section every iteration.
- Analyzer now derives q and averages r over the exact SYNC_SET-to-SYNC_SET interval.
- Supplied ESP01/ESP02 `no_rtc` profiles support the requested 30-minute A/B.
