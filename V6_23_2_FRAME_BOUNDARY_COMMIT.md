# v6.23.2 frame-boundary commit

## Symptom
A brief malformed artifact was visible only in the seconds-units digit at every one-second transition, especially on glyphs such as 5, 3, and 0. Disconnecting the external analyzer did not remove it.

## Root cause
The v6.23.1 refresh loop already latched one framebuffer index for a complete HUB75 scan, so the scanner itself did not reread the index row-by-row. The remaining race was buffer reclamation:

1. The exact-second ISR changed `active_frame_index` immediately.
2. Core 1 could still be finishing the previous scan from the old framebuffer.
3. The display task returned from the ISR publication wait and immediately rendered the following second.
4. `clear()` treated the old framebuffer as inactive and could clear/repaint it before core 1 had finished scanning it.

That allows one physical scan to contain pixels that are being rewritten, producing a short hybrid/malformed digit.

## Fix
- ISR publication writes `pending_frame_index` + `pending_frame_valid`; it does not change the scan-active buffer.
- At the top of the refresh loop, after a complete scan has ended, core 1 atomically adopts the pending frame and toggles GPIO16.
- `clear()` waits until the pending frame has been adopted before recycling the old scan buffer.

This preserves two buffers while guaranteeing that a buffer being scanned is immutable for the entire scan.

## Diagnostics
- GPIO33: logical exact-second publication request.
- GPIO16: physical complete-scan adoption.
- Expected GPIO33→GPIO16 delay: one refresh period or less.
- `frame_not_ready` semantics are unchanged.

## Unchanged
DS3231/SQW v6.23.1, synchronization, controller protocol, START/degraded gates, network-silent RUNNING, countdown color, and timing calculations are unchanged.
