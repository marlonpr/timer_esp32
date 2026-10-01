# v6.23.2 display artifact test

## Purpose
Verify that seconds-unit digit transitions no longer show the brief malformed/hybrid artifact seen on v6.23.1.

## First test — no analyzer required
1. Flash v6.23.2 on ESP01 and ESP02.
2. Keep the analyzer disconnected.
3. Confirm boot logs contain:
   - `frame_commit=refresh_boundary`
   - `active_frame_publish=pending_refresh_boundary`
   - DS3231/SQW still reports `isr_core=1` / `sqw_core=1`.
4. Start a 60-second countdown and watch the seconds-units digit closely, especially transitions involving 5, 3, and 0.
5. Expected result: no malformed intermediate glyph or tear on any one-second transition.

## Regression checks
- countdown remains red including 00:00
- `frame_not_ready=0`
- no missed/skipped second
- RTC remains LOCKED
- `queue_drops=0`
- `sqw_core=1`

## Optional analyzer check
Only if desired. GPIO33 remains the logical exact-second publication marker and GPIO16 is now the physical frame-boundary adoption marker. GPIO16 should occur within one refresh period after GPIO33. The analyzer is not required for the visual acceptance test.
