# Factory timer — hybrid 5-device firmware

This package is prepared for **2 classic ESP32 + 3 ESP32-S3**:

- ESP01, ESP02: `esp32`
- ESP03, ESP04, ESP05: `esp32s3`

Run `configure_hybrid_profiles.ps1` once from an ESP-IDF PowerShell terminal before building the S3 profiles. See `HYBRID_5_DEVICE_GUIDE.md`.

---

# ESP32 factory timer synchronization test firmware

ESP-IDF firmware for the classic ESP32 used by the Windows factory-timer synchronization proof of concept.

Key behavior in this revision:

- Infrastructure Wi-Fi and UDP port 5000.
- `esp_timer_get_time()` remains the local monotonic clock; it is never reset.
- FCT2 NTP-style `SYNC` / `SYNC_REPLY` / `SYNC_SET` clock-offset synchronization.
- FCT2 absolute `START_AT` scheduling against a future Master timestamp.
- Duplicate command IDs cannot move an armed target.
- `START_AT` is rejected as `NOT_SYNCED` or `LATE` when appropriate.
- `STARTED` telemetry reports the actual observed start time in both local and reconstructed Master time.
- Wi-Fi power saving is disabled to reduce timing jitter.
- Precise countdown starts are driven by a dedicated deadline task, not UDP busy polling.
- `udp_command` uses a 50 ms receive timeout so it blocks cleanly and no longer starves the FreeRTOS idle task.
- A dedicated `countdown_timer` task performs the Armed -> Running transition and records the actual local start timestamp.
- STARTED telemetry is sent later by the UDP task using the timestamp captured at the precise deadline transition.

The portable timing/protocol files are exercised by `tests/firmware-host`.

Build every unit from this same firmware source using a separate sdkconfig/build directory. For the five-device test use the exact IDs `ESP01`, `ESP02`, `ESP03`, `ESP04`, and `ESP05`. The package includes `sdkconfig.esp01` through `sdkconfig.esp05`; Wi-Fi settings can be edited per device as needed. Do not reuse one CMake build directory for multiple sdkconfig profiles.

## Factory-LAN validation / bad-IP rejection

This revision rejects DHCP leases that are not on the expected factory LAN:

- IPv4 network: `192.168.0.0/16`
- Netmask: `255.255.0.0`
- Gateway: `192.168.0.1`

For example, `10.45.0.175 / 255.255.0.0 / gateway 10.45.0.245` is rejected.
The UDP command service remains disabled, the clock synchronization is invalidated,
and the firmware restarts the DHCP client after one second to request a fresh lease.
It repeats this automatically until an accepted lease is received. The ESP32 stays
associated to the configured Wi-Fi AP while DHCP is retried.


## Watchdog / START_AT scheduler revision

See `WATCHDOG_ESP_TIMER_FIX.md` for the architecture and hardware validation steps.
The local execution mechanism for the scheduled start was moved from continuous UDP polling to the validated deadline-task scheduler.


## START_AT scheduler

This test branch uses a dedicated deadline task for START_AT execution: it blocks until near the target and uses `esp_timer_get_time()` only for a short final precision window. See `DEADLINE_TASK_START_SCHEDULER.md`.


## Controlled SYNC path-delay experiment

This build accepts an optional artificial reply delay on FCT2 SYNC requests and is paired with the Windows controller test UI. See `SYNC_PATH_DELAY_EXPERIMENT.md`.


## Five-device fleet test

No firmware logic change is required when scaling from two to five devices. Each board runs the same binary source; only its configured identity and, if needed, its Wi-Fi credentials differ. Use the exact IDs `ESP01` ... `ESP05`. Keep separate build directories (`build-esp01` ... `build-esp05`) so CMake never reuses a cached SDKCONFIG path. The Windows five-device controller discovers all five IDs, synchronizes them sequentially, and broadcasts one shared absolute `START_AT` target to the entire fleet.
