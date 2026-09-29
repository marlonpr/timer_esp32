# First v6.14 validation run

Use two classic ESP32 timers with v6.14, the ISR-validation controller, and the
level-aware analyzer.

## Wiring

- ESP01 GPIO33 -> analyzer GPIO4 (`ESP01_COMMIT`)
- ESP02 GPIO33 -> analyzer GPIO5 (`ESP02_COMMIT`)
- ESP01 GPIO16 -> analyzer GPIO6 (`ESP01_REFRESH`)
- ESP02 GPIO16 -> analyzer GPIO7 (`ESP02_REFRESH`)
- common GND between both timers and analyzer

GPIO33 toggles in the ISR immediately after the active-frame index publication.
GPIO16 toggles on core 1 when the refresh loop adopts that new index at the
start of a complete HUB75 scan cycle.

## Run

1. Wait for both RTC disciplines to be LOCKED.
2. `BEGIN|1|1` on the analyzer.
3. Run a 20-second countdown with the ISR-validation controller.
4. Let the run finish so buffered `ISR_PUBLISH`, `STATUS_PATH`, `STATUS_TX` and
   `SYNC_REPLY_TRACE` records dump.
5. End analyzer capture.

The controller intentionally keeps per-second STATE_CHANGE traffic and sweeps
STATUS requests through approximately boundary-6 ms to boundary+2 ms.

## Pass criteria for the publication layer

- `frame_not_ready=0`.
- `callback_lateness_us` / GPIO33 marker lateness do not develop millisecond
  tails when `recv_us` lands in the old hazardous window.
- GPIO33 lateness is independent of STATUS receive lead and stays in the
  tens-of-microseconds range targeted for this validation.
- GPIO16 follows each GPIO33 publication once, with a bounded publish-to-refresh
  adoption delay determined by HUB75 scan phase.

Useful commands:

```powershell
python tools\analyze_isr_publish.py ESP01.log --analyzer analyzer.txt --device ESP01_COMMIT
python tools\analyze_sync_sendto.py ESP01.log
```
