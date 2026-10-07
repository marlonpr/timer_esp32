#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the physical 64x32 HUB75 panel and the independent display
// scheduler task. The panel shows the common 64x32 logo while idle.
bool factory_display_init(const char* device_id, uint8_t brightness);

// Cache task identities once after all application/Wi-Fi tasks exist and before
// opening the command socket. No inventory or stack scan runs at ARM/RUNNING.
void factory_display_cache_runtime_inventory(void);

// Changes panel output brightness immediately without altering countdown state
// or timing. Value is an operator-facing percentage from 0 (off) to 100.
void factory_display_set_brightness_percent(uint8_t brightness_percent);

// Start accounting at command acceptance, before temperature suppression and ARM.
void factory_display_begin_run_monitor(uint64_t command_id, int64_t local_start_us);

// The immutable disciplined T* and its initial raw-local deadline come from ARM.
// Every scheduled boundary, including 0, uses T* + n * 1000000 us.
void factory_display_arm(int64_t local_start_us, int64_t start_disciplined_us,
                         uint32_t duration_seconds, uint64_t command_id);

// Cancels any pending/running visual countdown and returns to the idle frame.
void factory_display_reset(void);

// Compact per-run presentation health snapshot for controller-visible STATUS
// telemetry. Values belong to command_id when valid is true.
typedef struct {
    bool valid;
    uint64_t command_id;
    int64_t start_publish_lateness_us;
    int64_t worst_publish_lateness_us;
    uint32_t frame_not_ready_count;

    // v6.23.11 fleet CPU0 latency-monitor summary. These fields are valid
    // when cpu0_monitor_valid is true and belong to the same command_id.
    bool cpu0_monitor_valid;
    uint32_t cpu0_monitor_samples;
    uint32_t cpu0_monitor_missed_periods;
    uint32_t cpu0_monitor_event_count;
    uint32_t cpu0_monitor_worst_us;
    char cpu0_monitor_worst_task[16];
    uint32_t cpu0_commit_late_count;
    uint32_t cpu0_commit_worst_us;
    bool cpu0_commit_overlap;
    uint32_t cpu0_overlap_sample_us;
    uint32_t cpu0_overlap_commit_us;
    char cpu0_overlap_task[16];
    uint32_t cpu0_wrong_core_callbacks;
    uint32_t cpu0_monitor_overflow;
    uint32_t cpu0_commit_overflow;
    uint8_t cpu0_sampler_intr_level;
    uint8_t cpu0_commit_intr_level;
} factory_display_health_t;

// Thread-safe snapshot of the most recent armed/running display schedule.
void factory_display_get_health(factory_display_health_t* out_health);

#ifdef __cplusplus
}
#endif
