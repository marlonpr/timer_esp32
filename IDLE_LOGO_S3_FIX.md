# v7 S3 idle-logo fix

This revision fixes the v6 ESP32-S3 display regression without changing synchronization or countdown state logic.

## Root cause addressed

The v6 idle renderer walked the flash-resident 64x32 logo in the display task and issued 2048 individual `set_pixel()` calls. Prior S3 logs from this project already showed a cache/MMU fault in the same `draw_bitmap_rgb32 -> set_pixel` pattern. During ARM or RESET that long per-pixel path could prevent the display task from reaching the countdown/idle commit.

## v7 changes

- The supplied 64x32 `0xRRGGBB` logo is converted at build/source-generation time to packed RGB888 bytes.
- At display init it is copied once into a 6144-byte internal-DRAM buffer.
- ESP32-S3 draws the complete image into the HUB75 back buffer with one `Hub75Driver::draw_pixels(... RGB888 ...)` call.
- Classic ESP32 consumes the same RAM RGB888 buffer through its existing software framebuffer writer.
- Display task stack is increased from 4096 to 6144 bytes.
- Countdown digit rendering, double-buffer flip sequence, ARM/RESET state handling, START_AT timing, networking, sync, device IDs, and GPIO mappings are otherwise unchanged.

## Expected behavior

- Boot/READY: logo visible.
- ARMED before T*: logo remains visible while countdown first frame is prepared in the back buffer.
- At T*: green countdown becomes visible.
- FINISHED: red 00 remains visible, as in v5/v6.
- RESET: logo becomes visible again.
