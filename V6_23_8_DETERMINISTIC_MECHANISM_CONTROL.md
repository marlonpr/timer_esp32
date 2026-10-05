# v6.23.8 — deterministic cache / interrupt-mask mechanism control

## Why this revision exists

v6.23.7 positively validated the main `esp_flash` OS wrapper, but the three NVS writes did not prove that an actual cache-disabled window overlapped a COMMIT edge. Small NVS writes produced 18 cache-off windows totaling 733 us, each below 100 us. The long 13.2 ms `nvs_set_u32()` call at boundary 60 therefore did not imply that cache was disabled continuously across the boundary.

v6.23.8 removes that ambiguity. It does not rely on NVS timing probability.

## Diagnostic-only changes

Firmware synchronization, RTC discipline, display publication, frame-boundary adoption, and production network-silent RUNNING behavior are unchanged.

ESP02 only receives three deterministic controls during a 180-second countdown:

| Boundary | Control | Requested protected interval | Purpose |
|---:|---|---|---|
| 30 | direct cache-off | start 500 us early, hold 1000 us | prove whether the IRAM COMMIT path executes while cache is disabled |
| 60 | private CPU0 critical section | start 100 us early, hold 300 us | positive control: deliberately mask the CPU0 timer ISR and force a ~200 us late COMMIT |
| 90 | private CPU1 critical section | start 100 us early, hold 300 us | locality control: CPU1 masking should not delay CPU0 esp_timer COMMIT |

The cache-off control uses the ESP-IDF private diagnostic API:

```c
spi_flash_disable_interrupts_caches_and_other_cpu();
esp_rom_delay_us(1000);
spi_flash_enable_interrupts_caches_and_other_cpu();
```

This firmware is for qualification diagnostics only.

## Analyzer-visible blocking probe

ESP02 GPIO32 is driven HIGH for each protected interval. The ESP02 physical START-edge diagnostic is disabled in this diagnostic profile because it previously used GPIO32:

ESP02 GPIO32 is driven HIGH for each protected interval:

- every real wrapped `esp_flash` cache-off interval;
- boundary-30 injected cache-off hold;
- boundary-60 CPU0 critical section;
- boundary-90 CPU1 critical section.

GPIO writes use `GPIO_OUT_W1TS/W1TC` directly so the marker itself does not depend on flash-resident code.

For Analyzer_v8, disconnect the ESP02 physical SQW analyzer tap and reuse the analyzer input currently labeled **channel 4 / `ESP02_SQW`**:

```text
ESP02 GPIO32  --->  Analyzer_v8 channel 4 / ESP02_SQW input
ESP02 GPIO33  --->  existing ESP02_COMMIT input
ESP02 GPIO16  --->  existing ESP02_REFRESH input
GND           --->  common GND
```

Analyzer_v8 firmware does not need to change. In this run `ESP02_SQW` means `ESP02_BLOCK_PROBE`.

## Flash wrapper timing

The flash wrapper now retains every event (`threshold_us=0`) and separates:

```text
start_hook_enter_us
cache_off_begin_us
start_wait_us = cache_off_begin - start_hook_enter
cache_off_end_us
cache_off_duration_us
end_hook_return_us
end_restore_us = end_hook_return - cache_off_end
```

This distinguishes time spent waiting for the flash lock / other-core IPC from the actual cache-off interval.

## Expected physical signatures

### Boundary 30 — cache-off

The blocking-probe pulse should straddle the fitted COMMIT grid by roughly -500/+500 us.

If the COMMIT path is genuinely IRAM-safe:

- GPIO33 COMMIT remains on the normal ~1 us physical grid;
- the COMMIT edge occurs while GPIO32 is HIGH;
- firmware callback lateness stays near normal.

A panic would instead indicate that some callback dependency is flash-resident.

### Boundary 60 — CPU0 mask positive control

GPIO32 should be HIGH from roughly -100 to +200 us relative to the expected edge.

Because `CONFIG_ESP_TIMER_INTERRUPT_LEVEL=1`, the private CPU0 critical section should hold the timer ISR pending until the section exits. Expected:

- physical GPIO33 COMMIT roughly +200 us late;
- COMMIT occurs just after GPIO32 falls;
- firmware `callback_lateness_us` / `WorstPublishLatenessUs` agrees with Analyzer.

This is the required positive control that proves the rig detects a masking stall of the same order as the historical 358–800 us ESP02 events.

### Boundary 90 — CPU1 locality control

GPIO32 again spans roughly -100/+200 us, but the critical section runs on CPU1. With the esp_timer ISR pinned to CPU0:

- GPIO33 COMMIT should remain on grid and occur while GPIO32 is HIGH;
- CPU1 refresh timing may move; logical COMMIT should not.

## Run

Use the existing v6.22.8 controller and Analyzer_v8. A 3:00 countdown is sufficient.

After the run:

```powershell
python tools\analyze_v6238_mechanism_control.py `
    "ESP02_SERIAL.txt" `
    "Analyzer_v8.log"
```

Desired result:

```text
CACHE_OFF_IRAM_PATH_TEST=PASS
CPU0_MASK_POSITIVE_CONTROL=PASS
CPU1_LOCALITY_CONTROL=PASS
RESULT=PASS
```

If the CPU0 positive control passes and the cache-off/CPU1 controls stay on-grid, the old smooth 358–800 us ESP02 stalls should be investigated primarily as CPU0 interrupt masking / competing level-1 ISR behavior rather than ordinary cache-disable blocking.
