#pragma once

#include <cstdint>

/**
 * v6.23.8 diagnostic-only timing mechanism instrumentation.
 *
 * - Wraps the main esp_flash OS start/end callbacks and records hook-entry,
 *   cache-off, and restore timing with all events retained when threshold=0.
 * - Optionally exposes the protected interval on GPIO32 for Analyzer_v8.
 * - On ESP02 only, injects deterministic controls at disciplined boundaries:
 *     b30: 1 ms cache-off window centered on the edge
 *     b60: 300 us CPU0 critical section starting 100 us before the edge
 *     b90: 300 us CPU1 critical section starting 100 us before the edge
 *
 * Production countdown/synchronization behavior is otherwise unchanged.
 */
bool flash_guard_diag_init();
void flash_guard_diag_begin_run(uint64_t command_id, int64_t target_local_start_us);
void flash_guard_diag_countdown_started(uint64_t command_id, int64_t actual_local_start_us);
void flash_guard_diag_countdown_finished();
void flash_guard_diag_dump_run();
