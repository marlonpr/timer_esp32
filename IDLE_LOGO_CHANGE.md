# v6 idle-logo display change

This revision changes only the idle presentation of the 64x32 factory countdown panel.

- The previous steady numeric device identity (`01`..`05`) is replaced by the supplied 64x32 Bobcat RGB bitmap.
- The same shared renderer is used for both supported targets:
  - classic ESP32 software HUB75 backend;
  - ESP32-S3 LCD_CAM/GDMA HUB75 backend.
- The device ID remains unchanged in firmware configuration and network/protocol identity.
- Countdown rendering, START_AT timing, synchronization, RESET semantics, diagnostics, Wi-Fi/network policy, and target pin mappings are unchanged.
- The logo is rendered only where the previous idle/device-ID frame was rendered. This revision intentionally does not change the FINISHED red `00` behavior.

## Existing target profiles in this source tree

- `sdkconfig.esp01`: ESP32 classic
- `sdkconfig.esp02`: ESP32 classic
- `sdkconfig.esp03`: ESP32-S3
- `sdkconfig.esp04`: ESP32 classic
- `sdkconfig.esp05`: ESP32-S3

The full source tree supports both `esp32` and `esp32s3`; no separate display source fork is required.
