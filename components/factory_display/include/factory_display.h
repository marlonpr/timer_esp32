#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the physical 64x32 HUB75 panel and the independent display
// scheduler task. The panel shows the last two digits of device_id while idle.
bool factory_display_init(const char* device_id, uint8_t brightness);

// Arms the visual countdown against the SAME local deadline used by the
// countdown scheduler. The panel remains on the idle/device-ID frame until the
// deadline, then changes to duration_seconds and decrements once per second.
void factory_display_arm(int64_t local_start_us, uint32_t duration_seconds);

// Cancels any pending/running visual countdown and returns to the idle frame.
void factory_display_reset(void);

#ifdef __cplusplus
}
#endif
