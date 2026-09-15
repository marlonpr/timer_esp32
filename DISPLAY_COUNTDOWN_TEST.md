# Five-device 64x32 HUB75 countdown test

This branch adds one 64x32 P5 panel to each of ESP01..ESP05 without changing the FCT1/FCT2 protocol or Windows controller.

## Fleet hardware

- ESP01, ESP02, ESP04: classic ESP32 software-scanned HUB75 backend using the proven `1x1_p5_test` pinout:
  - R1=2 G1=4 B1=5 R2=18 G2=19 B2=25
  - A=15 B=26 C=23 LAT=12 OE=14 CLK=13
  - 64x32 four-scan/ABC mapping, 3-bit BCM
- ESP03, ESP05: ESP32-S3 GDMA backend using the actual runtime `make_config()` from `S3_P5_test`:
  - 64x32, `SCAN_1_8_32PX_FULL`, FM6126A, double buffer
  - R1=4 G1=5 B1=6 R2=7 G2=15 B2=16
  - A=11 B=12 C=13 D=-1 E=-1 LAT=9 OE=10 CLK=8
  - min refresh target 150 Hz

The physical START diagnostic remains on GPIO21. This does not conflict with either backend above.

## Display behavior

- Idle / RESET: blue device number (`01`..`05`).
- ARMED: device number remains visible. The first countdown frame is pre-rendered in the back buffer.
- Exact local START deadline: panel flips to the requested duration in green.
- Every subsequent one-second local deadline: panel decrements by one.
- Finish: red `00` remains visible until RESET/re-arm.
- Display is two-digit in this first visual test. Durations above 99 s are clamped visually to 99 until the real remaining time reaches 99; timer/protocol state is not clamped.

The display scheduler never draws inside the high-priority countdown TimerTask.

## Task placement for display-load validation

- TimerTask: priority 24, core 1.
- CommandTask: priority 15, core 1.
- Display deadline/render task: priority 12, core 1.
- Classic ESP32 software HUB75 refresh: priority 1, core 0.
- ESP32-S3 HUB75 refresh: LCD_CAM + GDMA; initialization occurs on core 0.
- Wi-Fi/lwIP keep their ESP-IDF priorities.

This deliberately leaves TimerTask above everything application-side and puts UDP command handling on the core opposite the display refresh engine.

For the classic ESP32 backend, software refresh intentionally occupies core 0 continuously. Instead of disabling TWDT globally, the refresh task removes IDLE0 from TWDT and subscribes itself, feeding once per complete scan frame.

## Brightness

`CONFIG_FACTORY_DISPLAY_BRIGHTNESS=128` by default.

- S3: direct 0..255 HUB75 brightness.
- ESP32: mapped to OE PWM percentage.

## First test sequence

1. Flash the correct existing profile (`sdkconfig.esp01` ... `sdkconfig.esp05`).
2. Confirm idle panels display blue `01`, `02`, `03`, `04`, `05`.
3. Keep the external START-edge analyzer connected on the same GPIO21 wiring.
4. Use the existing Windows controller and run a 20 s countdown.
5. All five panels should change from device ID to green `20` together, then 19..01, then red `00`.
6. Capture serial logs. Key new messages:
   - `64x32 countdown display ready`
   - `Visual countdown started ... flip_call_lateness_us=...`
   - `Visual countdown finished ... worst_flip_call_lateness_us=...`
7. Run the frozen 30-trial analyzer regression with panels continuously refreshing.

Do not compare new long-term clock-rate values directly to the old panel-free characterization until the panel-active ADEV/rate run is complete. This branch intentionally changes CPU/interrupt/power load even though CPU frequency remains at the existing timer profile value.

## Validation performed here

The existing firmware-host suite passes 2/2. Full ESP-IDF target builds were not available in the generation environment and must be run on the normal ESP-IDF Windows/macOS setup before flashing.


## CPU affinity revision: display-scan isolation

For the classic ESP32 software-scanned P5 backend, the scan/refresh task is now pinned to **CPU1**.
`udp_command` and the high-resolution countdown `TimerTask` are pinned to **CPU0**. The low-rate
countdown drawing/scheduler task is also on CPU0 so drawing does not preempt the CPU1 scan loop.
The task watchdog handoff follows the refresh task and therefore moves from IDLE0 to IDLE1.

This change targets flicker caused by the Wi-Fi/control workload interrupting the software HUB75
scan loop. On ESP32-S3 the panel refresh is GDMA/LCD_CAM based rather than a CPU software scan,
so CPU affinity is much less likely to be the cause of S3 flicker.


## v3 classic ESP32 flicker reduction

For ESP01 / ESP02 / ESP04 only:

- HUB75 software refresh remains pinned to CPU1.
- Refresh-task priority is raised from 1 to 20 so incidental CPU1 tasks cannot interrupt scan timing.
- CPU1 idle-task TWDT checking is disabled; CPU0 watchdog protection remains enabled.
- The refresh loop no longer calls `esp_task_wdt_reset()` once per display frame. At the ~hundreds-of-Hz scan rate that call itself was a periodic source of scan-loop jitter.
- Classic ESP32 profiles are set to 240 MHz, matching the successful manual test condition.

If visible flicker remains after this version, stop tuning software task affinity. The remaining disturbance is most likely interrupt-level jitter inherent to CPU-bit-banged HUB75 refresh while Wi-Fi is active. The architectural fix is the ESP32 I2S-DMA HUB75 backend, which refreshes from a circular DMA chain without CPU timing dependence.

## v4 digit-1 geometry

Digit `1` now uses the same vertical bounding box as digit `0` on the 64x32 panel:

- `0`: y = 1..29 (1 free pixel above, 2 below)
- `1`: y = 1..29 (1 free pixel above, 2 below)

Only digit `1` is changed. Its right-side upper/lower strokes are extended while the center gap remains unchanged. This affects both classic ESP32 and ESP32-S3 builds because they share `factory_display.cpp`.
