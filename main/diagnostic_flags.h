#pragma once

#include <cstdint>
#include "sdkconfig.h"

namespace factory_timer {

// STATUS build identity, independent of runtime HealthFlags. Observation-only
// COMMIT/refresh markers remain enabled in ordinary qualification profiles.
constexpr uint8_t BuildDiagnosticFlags() {
    uint8_t flags = 0;
#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
    flags |= 0x01;  // TASK_CANARY_ENABLED
#endif
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
    flags |= 0x02;  // ISR_CANARY_ENABLED
#endif
#if defined(CONFIG_FACTORY_START_TASK_TEST_DELAY_US) && CONFIG_FACTORY_START_TASK_TEST_DELAY_US > 0
    flags |= 0x04;  // START_TASK_DELAY_TEST
#endif
#if (defined(CONFIG_FACTORY_TIMING_PROBE_OUTPUT) && CONFIG_FACTORY_TIMING_PROBE_OUTPUT) || \
    (defined(CONFIG_FACTORY_TIMING_MECHANISM_TEST) && CONFIG_FACTORY_TIMING_MECHANISM_TEST) || \
    (defined(CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS) && CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS) || \
    (defined(CONFIG_FACTORY_POST_RUN_TIMING_DUMP) && CONFIG_FACTORY_POST_RUN_TIMING_DUMP) || \
    (defined(CONFIG_FACTORY_ANALYZER_PRESENTATION_OFFSET_US) && CONFIG_FACTORY_ANALYZER_PRESENTATION_OFFSET_US > 0)
    flags |= 0x08;  // GPIO_PROBE / timing diagnostic
#endif
#if defined(CONFIG_IDF_TARGET_ESP32) && CONFIG_IDF_TARGET_ESP32 && \
    (!defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) || !CONFIG_FACTORY_CPU0_LATENCY_MONITOR)
    flags |= 0x10;  // MONITOR_OFF_A_B (classic ESP32)
#endif
    return flags;
}

}  // namespace factory_timer
