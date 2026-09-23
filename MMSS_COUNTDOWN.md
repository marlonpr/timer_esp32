# MM:SS countdown display

This revision changes the visible countdown from two seconds-only digits to `MM:SS`.

- 64x32 HUB75 layout: four compact seven-segment digits plus colon.
- Maximum intended controller duration: `99:59` (5999 seconds).
- RUNNING: green `MM:SS`.
- FINISHED: red `00:00`.
- READY / RESET / ARMED-before-T*: unchanged Bobcat idle logo.
- Brightness control is unchanged.
- FCT1/FCT2 command packets remain backward-compatible and still carry one total-second duration field.
- Both classic ESP32 and ESP32-S3 use the same shared `factory_display.cpp` MM:SS renderer.
