#pragma once

#include <cstdint>

bool cpu0_latency_monitor_init();
void cpu0_latency_monitor_prepare_run(uint64_t command_id);
void cpu0_latency_monitor_begin_run(uint64_t command_id, int64_t start_disciplined_us);
void cpu0_latency_monitor_end_run();
void cpu0_latency_monitor_dump_run();
void cpu0_latency_monitor_set_commit_target(int64_t target_local_us);
void cpu0_latency_monitor_note_commit(uint32_t boundary,
                                                int64_t target_local_us,
                                                int64_t callback_entry_us,
                                                int64_t marker_begin_us,
                                                int64_t marker_end_us);
