# v6.12 inverse-mapping boundary trace

Purpose: determine whether the recurring 3-4 ms analyzer bursts come from the
DS3231 disciplined->local inverse mapping or occur after the computed deadline.

This build preserves v6.11b scheduling, marker-gap diagnostics, RX boundary
tracing, RTC lock gating, Wi-Fi settings, and network policy. It does not make
the GPIO33 marker atomic and does not enable the ISR-protected scheduler.

For each one-second countdown boundary, the display task records in RAM:

- `boundary`: 1..duration
- `disciplined_us`: exact disciplined-domain boundary
- `boundary_local_us`: the final local deadline actually returned by the wait
  loop and used for that boundary
- `step_us`: difference from the prior used local deadline
- `flip_commit_us`: raw `esp_timer_get_time()` immediately after backend flip
- `toggle_after_us`: raw local timestamp immediately after GPIO33 toggle
- `post_local_us`: a fresh disciplined->local conversion sampled after marker
- `post_minus_wait_us`: whether the inverse mapping changed across the boundary
- lateness against both the used deadline and the post-marker conversion

No `BOUNDARY_TRACE` UART lines are printed during the countdown. Up to 64
boundaries are buffered in RAM and dumped only after `Visual countdown finished`
so the trace itself cannot perturb the measured boundaries. Use a 20-60 second
run for this diagnostic.

For a device at about +4.3 ppm, regular local deadlines should increase by about
`1,000,004.3 us` per disciplined second. A several-millisecond change in
`boundary_local_us`/`step_us` at the analyzer burst proves the inverse mapping
moved the display target. A regular deadline series while the physical marker is
late pushes the investigation past the computed deadline.

Analyze a saved UART log with:

```
python tools/analyze_boundary_mapping.py ESP01.log
```
