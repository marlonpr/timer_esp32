# v6.14 — ISR frame publication + refresh adoption + SYNC send trace

Classic ESP32 countdown boundaries now use an ISR-dispatched `esp_timer` rather
than a priority-12 task spin. The next MM:SS frame is rendered into the inactive
framebuffer well before its deadline. At the local boundary the ISR:

1. checks the prepared-frame sequence,
2. publishes one aligned active-frame index,
3. toggles the GPIO33 commit marker with a direct GPIO register write,
4. timestamps the result and wakes the display task.

The core-1 HUB75 refresh loop snapshots the active index once per complete scan.
When it adopts a new index it toggles the refresh-adoption marker (GPIO16 by
default). This exposes publish -> first full scan adoption separately from the
software commit marker.

`ISR_PUBLISH` records are buffered until the run ends and include target, arm
margin, callback lateness, publish+marker duration, mapping recheck, sequence
status and cumulative frame-not-ready count.

START remains on the proven task/raw-local path. ESP32-S3 retains the previous
task/GDMA display path; the ISR publication experiment applies to classic ESP32.

The STATUS diagnostics and per-second `STATE_CHANGE` transmissions remain on for
the first validation, intentionally preserving the starvation stimulus.

SYNC replies now buffer `SYNC_REPLY_TRACE` samples with T2, T3, final format,
`sendto()` entry and return. They are dumped after the countdown so trace logging
does not perturb synchronization.

Useful tools:

```text
python tools/analyze_isr_publish.py ESP01.log --analyzer analyzer.txt --device ESP01
python tools/analyze_sync_sendto.py ESP01.log
```
