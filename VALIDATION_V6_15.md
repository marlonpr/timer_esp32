
## v6.15b compile portability fix

The SQW ISR diagnostic marker now supports classic ESP32 GPIO 0..33 without an invalid 32-bit left shift. GPIO 0..31 uses `GPIO.out_w1ts/out_w1tc`; GPIO 32..33 uses `GPIO.out1_w1ts/out1_w1tc`. The configured validation pin remains GPIO17.
