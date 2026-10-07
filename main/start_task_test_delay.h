#pragma once

#include <cstdint>
#include "sdkconfig.h"

#if defined(CONFIG_FACTORY_START_TASK_TEST_DELAY_US) && CONFIG_FACTORY_START_TASK_TEST_DELAY_US > 0
#include "esp_rom_sys.h"
#include "esp_timer.h"

namespace factory_timer {

// Diagnostic-only: called after the ordinary wait reaches the START target.
// Do not disable interrupts or change the armed display epoch.
inline int64_t InjectStartTaskTestDelay() {
    esp_rom_delay_us(CONFIG_FACTORY_START_TASK_TEST_DELAY_US);
    return esp_timer_get_time();
}

}  // namespace factory_timer
#endif
