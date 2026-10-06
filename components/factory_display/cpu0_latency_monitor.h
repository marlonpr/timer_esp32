#pragma once

#include <cstddef>
#include <cstdint>

constexpr std::size_t kCpu0LatencyTaskNameCapacity = 16;

struct cpu0_latency_monitor_summary_t {
    bool valid{};
    uint64_t command_id{};
    uint32_t sample_callbacks{};
    uint32_t missed_periods{};
    uint32_t event_count{};
    uint32_t worst_lateness_us{};
    char worst_task[kCpu0LatencyTaskNameCapacity]{};
    uint32_t commit_late_count{};
    uint32_t worst_commit_lateness_us{};
    bool commit_overlap{};
    uint32_t overlap_sample_lateness_us{};
    uint32_t overlap_commit_lateness_us{};
    char overlap_task[kCpu0LatencyTaskNameCapacity]{};
    uint32_t wrong_core_callbacks{};
    uint32_t event_overflow{};
    uint32_t commit_overflow{};
    uint8_t sampler_intr_level{};
    uint8_t commit_intr_level{};
};

bool cpu0_latency_monitor_init();
void cpu0_latency_monitor_prepare_run(uint64_t command_id);
void cpu0_latency_monitor_begin_run(uint64_t command_id, int64_t start_disciplined_us);
void cpu0_latency_monitor_end_run();
void cpu0_latency_monitor_dump_run();
void cpu0_latency_monitor_set_commit_target(uint32_t boundary, int64_t target_local_us);
void cpu0_latency_monitor_note_commit(uint32_t boundary,
                                      int64_t target_local_us,
                                      int64_t callback_entry_us,
                                      int64_t marker_begin_us,
                                      int64_t marker_end_us);
void cpu0_latency_monitor_get_summary(cpu0_latency_monitor_summary_t* out_summary);
