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

// Arms the visual countdown against the SAME local deadline used by the
// countdown scheduler. The panel remains on the idle/device-ID frame until the
// deadline, then changes to MM:SS and decrements once per second.
void factory_display_arm(int64_t local_start_us, uint32_t duration_seconds);

// Cancels any pending/running visual countdown and returns to the idle frame.
void factory_display_reset(void);

#ifdef __cplusplus
}
#endif
