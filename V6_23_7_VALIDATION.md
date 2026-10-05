# v6.23.7 validation performed in this environment

Source/static checks performed here:

- `tools/test_v6235_flash_guard_diagnostics.py`: PASS 10/10
- `tools/test_v6236_induced_nvs_positive_control.py`: PASS 9/9
- `tools/test_v6237_flash_os_targeted_positive_control.py`: PASS 24/24
- `tools/test_v6232_frame_boundary.py`: PASS
- `tools/test_v6233_health_telemetry.py`: PASS
- `tools/test_v6234_qualification_guards.py`: PASS
- `python -m py_compile tools/analyze_targeted_nvs_positive_control.py`: PASS

Important source contracts:

- main `esp_flash` OS callback path is wrapped; legacy/global guard setter is not used;
- all installed OS callbacks are copied and only start/end are replaced;
- start/end wrappers are IRAM and contain no logging or NVS calls;
- official ESP-IDF flash counters are enabled on ESP01/ESP02;
- ESP02 only performs induced NVS writes;
- induced `nvs_commit` targets disciplined boundaries 30/60/90;
- ESP01 remains the unstressed control;
- validated countdown scheduling/publish architecture is unchanged.

Not run in this environment:

- ESP-IDF v6.0 target build/flash;
- real v6.23.7 positive-control hardware test.
