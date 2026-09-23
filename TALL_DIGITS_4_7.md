# Taller countdown digits 4 and 7

This revision changes only the shared 64x32 `MM:SS` countdown glyph geometry.

- Digit `1` keeps its existing extended vertical strokes (`y=1..12` and `y=18..29`).
- Digit `4` now uses the same full visual height: its upper left/right strokes extend to `y=1`, and its lower-right stroke extends to `y=29`.
- Digit `7` now uses the same full visual height: its right-hand upper/lower strokes use the same geometry as digit `1`.
- The middle gap, digit widths, colon, colors, brightness, countdown timing, START_AT scheduling, networking, synchronization and logo are unchanged.
- The renderer is shared by ESP32 classic and ESP32-S3, so both targets receive the same glyph change.
