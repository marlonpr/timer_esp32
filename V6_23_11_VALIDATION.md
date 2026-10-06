# v6.23.11 validation

Validated in the assistant environment:

- firmware host protocol/core tests: PASS (2/2)
- v6.23.2 frame-boundary source contract: PASS
- v6.23.3 fleet-health source contract: PASS
- v6.23.4 qualification-guard source contract: PASS
- v6.23.1 SQW/core-affinity source contract: PASS
- GCC15 duplicate-IRAM declaration regression: PASS (3/3)
- v6.23.11 fleet-monitor source contract: PASS (40/40)
- Python tools compile: PASS
- STATUS conservative packet-envelope test: PASS (<=511 bytes)

The assistant environment does not contain ESP-IDF v6.0 target toolchains, so the
final ESP32 target build must be run on the Windows development machine.
