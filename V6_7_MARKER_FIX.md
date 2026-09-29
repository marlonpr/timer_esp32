# v6.7 presentation-marker fix

This validation-only revision keeps the DS3231 discipline and START scheduling from v6.6 unchanged.

## Marker change

The presentation diagnostic no longer toggles level once per commit. Instead it emits a 20 us positive pulse immediately after every software presentation commit:

- START frame
- every 1-second countdown frame
- final 00:00 frame

Classic ESP32 supplied profiles: GPIO33.
ESP32-S3 supplied profiles: GPIO14.

A 20-second countdown therefore produces 21 positive pulses per device.

The pulse occurs after `factory_display_backend_flip()` returns, so its rising edge is the presentation-commit timestamp. The 20 us pulse width occurs after that timestamp and does not move the commit deadline.

## Analyzer

Use Analyzer_S3_per_second_v3. It captures positive edges only and rejects additional edges occurring within 100 ms on the same input. This suppresses cable/contact ringing while preserving the intended ~1 Hz marker pulses.

Always connect a direct common ground between analyzer and every timer under test.
