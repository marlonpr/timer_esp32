#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the physical 64x32 HUB75 panel and the independent display
// scheduler task. The panel shows the common 64x32 logo while idle.
bool factory_display_init(const char* device_id, uint8_t brightness);

// Changes panel output brightness immediately without altering countdown state
// or timing. Value is an operator-facing percentage from 0 (off) to 100.
void factory_display_set_brightness_percent(uint8_t brightness_percent);

// Arms the visual countdown against the SAME raw local START deadline used by
// the countdown scheduler. START itself is unchanged. After START, each visible
// one-second boundary is scheduled in the DS3231-disciplined time domain and
// converted back to the current local esp_timer deadline.
void factory_display_arm(int64_t local_start_us, uint32_t duration_seconds,
                         uint64_t command_id);

// Publishes the exact disciplined START epoch captured by the high-priority
// countdown TimerTask. This does not control the first visible START frame; it
// only anchors post-START one-second boundaries.
void factory_display_note_started(uint64_t command_id,
                                  int64_t start_disciplined_us);

// Cancels any pending/running visual countdown and returns to the idle frame.
void factory_display_reset(void);

#ifdef __cplusplus
}
#endif
