#include "factory_display.h"
#include "display_backend.h"
#include "logo_bitmap.h"
#include "rtc_discipline.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32
#include "soc/gpio_struct.h"
#endif

namespace {

constexpr char kTag[] = "factory_display";
// S3 refresh is GDMA-driven, so put the presentation scheduler on CPU1 where
// it is isolated from TimerTask/CommandTask/Wi-Fi on CPU0. Classic ESP32 keeps
// the display scheduler on CPU0 because CPU1 is dedicated to software HUB75
// refresh.
#if CONFIG_IDF_TARGET_ESP32S3
constexpr BaseType_t kDisplaySchedulerCore = 1;
#else
constexpr BaseType_t kDisplaySchedulerCore = 0;
#endif
constexpr UBaseType_t kDisplaySchedulerPriority = 12;
constexpr int64_t kFineLeadUs = 2000;
constexpr int64_t kTickUs = 1000000LL / configTICK_RATE_HZ;
constexpr uint32_t kMaxDisplayedSeconds = 99u * 60u + 59u;

constexpr int64_t kMarkerGapThresholdUs = 100;
constexpr size_t kMaxRuntimeTasks = 32;
constexpr size_t kMaxReportedRuntimeDeltas = 16;
constexpr UBaseType_t kRuntimeDiagnosticMinPriority = 12;
constexpr size_t kCommitDelayDiagnosticCapacity = 16;
constexpr size_t kMarkerGapDiagnosticCapacity = 16;
constexpr int64_t kCommitDelayThresholdUs = 100;
constexpr int64_t kRuntimeSnapshotMinimumMarginUs = 1500;
// Diagnostic-only trace capacity. The intended inverse-mapping test is 20-60 s;
// records are buffered in RAM and dumped only after the run so UART logging
// cannot perturb the boundary being measured.
constexpr size_t kBoundaryTraceCapacity = 64;

#if CONFIG_IDF_TARGET_ESP32
constexpr size_t kIsrPublishTraceFirstCapacity = 8;
constexpr size_t kIsrPublishTraceWorstCapacity = 8;
constexpr size_t kIsrPublishTraceCapacity =
    kIsrPublishTraceFirstCapacity + kIsrPublishTraceWorstCapacity;
#if !defined(CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD) || \
    !CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD
#error "v6.14 ISR frame publication requires CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD=y"
#endif
#endif

#if defined(CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS && \
    defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY && \
    defined(CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#define FACTORY_MARKER_GAP_RUNTIME_DIAGNOSTICS 1
#else
#define FACTORY_MARKER_GAP_RUNTIME_DIAGNOSTICS 0
#endif

struct DisplayCommand {
    enum class Type : uint8_t { Arm, Reset } type{Type::Reset};
    int64_t local_start_us{};
    uint32_t duration_seconds{};
    uint64_t command_id{};
};

struct DisplayStartAnchor {
    uint64_t command_id{};
    int64_t start_disciplined_us{};
};

struct BoundaryTraceRecord {
    uint32_t boundary{};
    int64_t boundary_disciplined_us{};
    // Exact local deadline returned by WaitUntilDisciplinedOrReplacement().
    // This is the local deadline the display task actually waited on.
    int64_t wait_local_deadline_us{};
    // Timestamp taken when the display task enters its final precision window.
    // This distinguishes a late wake from starvation/blocking after wake.
    int64_t precision_entry_us{};
    int64_t runtime_snapshot_before_begin_us{};
    int64_t runtime_snapshot_before_end_us{};
    int64_t flip_request_us{};
    int64_t flip_commit_us{};
    int64_t toggle_after_us{};
    // Fresh inverse conversion sampled only after the marker. This preserves
    // the old diagnostic reference and exposes any mapping change across the
    // boundary operation.
    int64_t post_local_deadline_us{};
};

#if CONFIG_IDF_TARGET_ESP32
struct IsrPublishTraceRecord {
    uint32_t boundary{};
    uint32_t sequence{};
    int64_t disciplined_us{};
    int64_t target_local_us{};
    int64_t arm_us{};
    int64_t callback_entry_us{};
    int64_t publish_marker_begin_us{};
    int64_t publish_marker_end_us{};
    int64_t callback_exit_us{};
    int64_t post_local_us{};
    uint32_t prepared_sequence{};
    uint32_t published_sequence{};
    uint32_t frame_not_ready_count{};
    uint8_t marker_level{};
    bool published{};
};
#endif


#if FACTORY_MARKER_GAP_RUNTIME_DIAGNOSTICS
struct RuntimeTaskRef {
    TaskHandle_t handle{};
    char name[configMAX_TASK_NAME_LEN]{};
    BaseType_t core_id{tskNO_AFFINITY};
    UBaseType_t priority{};
};

struct RuntimeSnapshot {
    bool valid{false};
    int64_t capture_begin_us{};
    int64_t captured_us{};
    size_t count{};
    configRUN_TIME_COUNTER_TYPE counters[kMaxRuntimeTasks]{};
};

struct RuntimeTaskDelta {
    char name[configMAX_TASK_NAME_LEN]{};
    BaseType_t core_id{tskNO_AFFINITY};
    UBaseType_t priority{};
    uint64_t runtime_delta_us{};
};

struct MarkerGapDiagnostic {
    uint32_t boundary{};
    int64_t target_local_us{};
    int64_t flip_commit_us{};
    int64_t toggle_before_us{};
    int64_t toggle_after_us{};
    int64_t stats_before_us{};
    int64_t stats_after_us{};
    uint32_t diag_queue_drops{};
    uint8_t delta_count{};
    RuntimeTaskDelta deltas[kMaxReportedRuntimeDeltas]{};
};

struct CommitDelayDiagnostic {
    uint32_t boundary{};
    int64_t target_local_us{};
    int64_t precision_entry_us{};
    int64_t runtime_before_begin_us{};
    int64_t runtime_before_end_us{};
    int64_t flip_request_us{};
    int64_t flip_commit_us{};
    int64_t toggle_after_us{};
    int64_t runtime_after_begin_us{};
    int64_t runtime_after_end_us{};
    bool runtime_valid{};
    uint8_t delta_count{};
    RuntimeTaskDelta deltas[kMaxReportedRuntimeDeltas]{};
};

struct PrecisionWindowTrace {
    int64_t entry_us{};
    RuntimeSnapshot runtime_before{};
};
#else
struct RuntimeSnapshot {
    bool valid{false};
    int64_t capture_begin_us{};
    int64_t captured_us{};
};
struct PrecisionWindowTrace {
    int64_t entry_us{};
    RuntimeSnapshot runtime_before{};
};
#endif

QueueHandle_t s_command_queue = nullptr;
QueueHandle_t s_start_anchor_queue = nullptr;
TaskHandle_t s_display_task = nullptr;
std::atomic<bool> s_ready{false};

std::atomic<uint32_t> s_presentation_diagnostic_level{0};
BoundaryTraceRecord s_boundary_trace[kBoundaryTraceCapacity]{};
size_t s_boundary_trace_count = 0;
bool s_boundary_trace_overflow = false;

#if CONFIG_IDF_TARGET_ESP32
esp_timer_handle_t s_boundary_publish_timer = nullptr;
std::atomic<uint32_t> s_prepared_sequence{0};
std::atomic<uint32_t> s_scheduled_sequence{0};
std::atomic<uint32_t> s_published_sequence{0};
std::atomic<uint32_t> s_frame_not_ready_count{0};
std::atomic<uint32_t> s_publish_sequence_counter{0};
IsrPublishTraceRecord s_isr_last_sample{};
IsrPublishTraceRecord s_isr_publish_trace[kIsrPublishTraceCapacity]{};
size_t s_isr_publish_trace_first_count = 0;
size_t s_isr_publish_trace_worst_count = 0;
uint32_t s_isr_publish_trace_total = 0;
bool s_isr_publish_trace_overflow = false;
#endif

#if FACTORY_MARKER_GAP_RUNTIME_DIAGNOSTICS
CommitDelayDiagnostic s_commit_delay_diagnostics[kCommitDelayDiagnosticCapacity]{};
size_t s_commit_delay_diagnostic_count = 0;
bool s_commit_delay_diagnostic_overflow = false;
#endif

#if FACTORY_MARKER_GAP_RUNTIME_DIAGNOSTICS
RuntimeTaskRef s_runtime_tasks[kMaxRuntimeTasks]{};
size_t s_runtime_task_count = 0;
std::atomic<bool> s_runtime_inventory_ready{false};
MarkerGapDiagnostic s_marker_gap_diagnostics[kMarkerGapDiagnosticCapacity]{};
size_t s_marker_gap_diagnostic_count = 0;
bool s_marker_gap_diagnostic_overflow = false;
#endif

void SetPresentationDiagnosticEdge(bool high) {
#if defined(CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS
    s_presentation_diagnostic_level.store(high ? 1U : 0U, std::memory_order_relaxed);
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_FACTORY_DISPLAY_EDGE_GPIO), high ? 1 : 0);
#else
    (void)high;
#endif
}

void TogglePresentationDiagnosticEdge() {
#if defined(CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS
    const bool high = s_presentation_diagnostic_level.load(std::memory_order_relaxed) == 0U;
    SetPresentationDiagnosticEdge(high);
#endif
}

#if CONFIG_IDF_TARGET_ESP32
void IRAM_ATTR TogglePresentationDiagnosticEdgeFromIsr() {
#if defined(CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS
    const uint32_t next = s_presentation_diagnostic_level.load(std::memory_order_relaxed) ^ 1U;
    s_presentation_diagnostic_level.store(next, std::memory_order_relaxed);
#if CONFIG_FACTORY_DISPLAY_EDGE_GPIO < 32
    constexpr uint32_t kMask = 1U << CONFIG_FACTORY_DISPLAY_EDGE_GPIO;
    if (next != 0U) GPIO.out_w1ts = kMask; else GPIO.out_w1tc = kMask;
#else
    constexpr uint32_t kMask = 1U << (CONFIG_FACTORY_DISPLAY_EDGE_GPIO - 32);
    if (next != 0U) GPIO.out1_w1ts.val = kMask; else GPIO.out1_w1tc.val = kMask;
#endif
#endif
}
#endif


#if CONFIG_IDF_TARGET_ESP32
bool TryReceiveReplacement(DisplayCommand* replacement);

void ResetIsrPublishTrace() {
    s_isr_publish_trace_first_count = 0;
    s_isr_publish_trace_worst_count = 0;
    s_isr_publish_trace_total = 0;
    s_isr_publish_trace_overflow = false;
    s_prepared_sequence.store(0, std::memory_order_release);
    s_scheduled_sequence.store(0, std::memory_order_release);
    s_published_sequence.store(0, std::memory_order_release);
    s_frame_not_ready_count.store(0, std::memory_order_release);
}

void CancelBoundaryPublishTimer() {
    if (s_boundary_publish_timer != nullptr && esp_timer_is_active(s_boundary_publish_timer)) {
        (void)esp_timer_stop(s_boundary_publish_timer);
    }
    s_scheduled_sequence.store(0, std::memory_order_release);
    s_prepared_sequence.store(0, std::memory_order_release);
}

void IRAM_ATTR BoundaryPublishTimerCallback(void*) {
    const uint32_t sequence = s_scheduled_sequence.load(std::memory_order_acquire);
    const uint32_t prepared = s_prepared_sequence.load(std::memory_order_acquire);
    const int64_t callback_entry_us = esp_timer_get_time();

    // Write the shared sample field-by-field. Avoid aggregate initialization or
    // struct assignment in this IRAM callback so the compiler cannot lower the
    // operation to a flash-resident memset/memcpy helper.
    s_isr_last_sample.sequence = sequence;
    s_isr_last_sample.callback_entry_us = callback_entry_us;
    s_isr_last_sample.prepared_sequence = prepared;
    s_isr_last_sample.publish_marker_begin_us = 0;
    s_isr_last_sample.publish_marker_end_us = 0;
    s_isr_last_sample.published_sequence =
        s_published_sequence.load(std::memory_order_acquire);
    s_isr_last_sample.marker_level = static_cast<uint8_t>(
        s_presentation_diagnostic_level.load(std::memory_order_relaxed));
    s_isr_last_sample.published = false;

    if (sequence != 0U && prepared == sequence) {
        // Keep the visible publication and the physical marker adjacent. The
        // timestamp covers the combined active-index store + bare GPIO write.
        const int64_t begin_us = esp_timer_get_time();
        factory_display_backend_publish_prepared_from_isr();
        TogglePresentationDiagnosticEdgeFromIsr();
        const int64_t end_us = esp_timer_get_time();
        s_published_sequence.store(sequence, std::memory_order_release);
        s_isr_last_sample.publish_marker_begin_us = begin_us;
        s_isr_last_sample.publish_marker_end_us = end_us;
        s_isr_last_sample.published_sequence = sequence;
        s_isr_last_sample.marker_level = static_cast<uint8_t>(
            s_presentation_diagnostic_level.load(std::memory_order_relaxed));
        s_isr_last_sample.published = true;
    } else {
        s_frame_not_ready_count.fetch_add(1U, std::memory_order_relaxed);
    }

    s_isr_last_sample.frame_not_ready_count =
        s_frame_not_ready_count.load(std::memory_order_relaxed);
    s_isr_last_sample.callback_exit_us = esp_timer_get_time();

    TaskHandle_t task = s_display_task;
    if (task != nullptr) {
        BaseType_t higher_priority_woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &higher_priority_woken);
        if (higher_priority_woken == pdTRUE) {
            esp_timer_isr_dispatch_need_yield();
        }
    }
}

uint32_t MakePublishSequence(uint32_t boundary) {
    (void)boundary;
    uint32_t next = s_publish_sequence_counter.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (next == 0U) {
        next = s_publish_sequence_counter.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }
    return next;
}

void MarkPreparedFrame(uint32_t sequence) {
    std::atomic_thread_fence(std::memory_order_release);
    s_prepared_sequence.store(sequence, std::memory_order_release);
}

bool ArmBoundaryPublish(uint32_t boundary,
                        uint32_t sequence,
                        int64_t disciplined_us,
                        int64_t target_local_us,
                        int64_t* arm_us_out) {
    if (s_boundary_publish_timer == nullptr) return false;
    if (esp_timer_is_active(s_boundary_publish_timer)) {
        (void)esp_timer_stop(s_boundary_publish_timer);
    }

    IsrPublishTraceRecord seed{};
    seed.boundary = boundary;
    seed.sequence = sequence;
    seed.disciplined_us = disciplined_us;
    seed.target_local_us = target_local_us;
    seed.prepared_sequence = s_prepared_sequence.load(std::memory_order_acquire);
    s_isr_last_sample = seed;
    s_scheduled_sequence.store(sequence, std::memory_order_release);

    // esp_timer_start_once() takes a relative timeout. Sample as close as
    // possible to the call so setup work above is not added to the boundary.
    const int64_t arm_us = esp_timer_get_time();
    if (arm_us_out != nullptr) *arm_us_out = arm_us;
    s_isr_last_sample.arm_us = arm_us;
    int64_t delay_us = target_local_us - arm_us;
    if (delay_us < 1) {
        // The frame completed, but not before its intended boundary. Count this
        // as a frame-not-ready-at-deadline event even though we still publish
        // it immediately so the display can recover on the next frame.
        s_frame_not_ready_count.fetch_add(1U, std::memory_order_relaxed);
        delay_us = 1;
    }

    const esp_err_t result = esp_timer_start_once(
        s_boundary_publish_timer, static_cast<uint64_t>(delay_us));
    if (result != ESP_OK) {
        s_scheduled_sequence.store(0, std::memory_order_release);
        ESP_LOGE(kTag, "Boundary publish timer arm failed: boundary=%u error=%s",
                 static_cast<unsigned>(boundary), esp_err_to_name(result));
        return false;
    }
    return true;
}

bool WaitForBoundaryPublishOrReplacement(uint32_t sequence,
                                         DisplayCommand* replacement) {
    while (true) {
        if (TryReceiveReplacement(replacement)) {
            CancelBoundaryPublishTimer();
            return false;
        }

        const uint32_t published = s_published_sequence.load(std::memory_order_acquire);
        const IsrPublishTraceRecord sample = s_isr_last_sample;
        if (published == sequence || (sample.sequence == sequence && sample.callback_exit_us > 0)) {
            return true;
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
}

int64_t IsrPublishCallbackLateness(const IsrPublishTraceRecord& r) {
    return r.callback_entry_us - r.target_local_us;
}

void RetainIsrPublishTraceRecord(const IsrPublishTraceRecord& sample) {
    s_isr_publish_trace_total++;

    if (s_isr_publish_trace_first_count < kIsrPublishTraceFirstCapacity) {
        s_isr_publish_trace[s_isr_publish_trace_first_count++] = sample;
        return;
    }

    const size_t worst_base = kIsrPublishTraceFirstCapacity;
    if (s_isr_publish_trace_worst_count < kIsrPublishTraceWorstCapacity) {
        s_isr_publish_trace[worst_base + s_isr_publish_trace_worst_count++] = sample;
        return;
    }

    s_isr_publish_trace_overflow = true;
    size_t minimum_slot = worst_base;
    int64_t minimum_lateness = IsrPublishCallbackLateness(s_isr_publish_trace[minimum_slot]);
    for (size_t i = 1; i < kIsrPublishTraceWorstCapacity; ++i) {
        const size_t slot = worst_base + i;
        const int64_t lateness = IsrPublishCallbackLateness(s_isr_publish_trace[slot]);
        if (lateness < minimum_lateness) {
            minimum_lateness = lateness;
            minimum_slot = slot;
        }
    }
    if (IsrPublishCallbackLateness(sample) > minimum_lateness) {
        s_isr_publish_trace[minimum_slot] = sample;
    }
}

void RecordIsrPublishTrace(uint32_t boundary,
                           uint32_t sequence,
                           int64_t disciplined_us,
                           int64_t target_local_us,
                           int64_t arm_us,
                           int64_t post_local_us) {
    IsrPublishTraceRecord sample = s_isr_last_sample;
    sample.boundary = boundary;
    sample.sequence = sequence;
    sample.disciplined_us = disciplined_us;
    sample.target_local_us = target_local_us;
    sample.arm_us = arm_us;
    sample.post_local_us = post_local_us;
    RetainIsrPublishTraceRecord(sample);
}

void LogIsrPublishTraceRecord(const IsrPublishTraceRecord& r, const char* retention) {
    ESP_LOGI(kTag,
             "ISR_PUBLISH boundary=%u sequence=%u disciplined_us=%lld target_local_us=%lld arm_us=%lld arm_margin_us=%lld callback_entry_us=%lld callback_lateness_us=%lld publish_marker_begin_us=%lld publish_marker_end_us=%lld publish_marker_us=%lld callback_exit_us=%lld post_local_us=%lld post_minus_target_us=%lld prepared_sequence=%u published_sequence=%u published=%u marker_level=%u frame_not_ready_count=%u retention=%s",
             static_cast<unsigned>(r.boundary),
             static_cast<unsigned>(r.sequence),
             static_cast<long long>(r.disciplined_us),
             static_cast<long long>(r.target_local_us),
             static_cast<long long>(r.arm_us),
             static_cast<long long>(r.target_local_us - r.arm_us),
             static_cast<long long>(r.callback_entry_us),
             static_cast<long long>(r.callback_entry_us - r.target_local_us),
             static_cast<long long>(r.publish_marker_begin_us),
             static_cast<long long>(r.publish_marker_end_us),
             static_cast<long long>(r.publish_marker_end_us - r.publish_marker_begin_us),
             static_cast<long long>(r.callback_exit_us),
             static_cast<long long>(r.post_local_us),
             static_cast<long long>(r.post_local_us - r.target_local_us),
             static_cast<unsigned>(r.prepared_sequence),
             static_cast<unsigned>(r.published_sequence),
             r.published ? 1u : 0u,
             static_cast<unsigned>(r.marker_level),
             static_cast<unsigned>(r.frame_not_ready_count),
             retention);
}

void DumpIsrPublishTrace() {
    const size_t retained_count =
        s_isr_publish_trace_first_count + s_isr_publish_trace_worst_count;
    ESP_LOGI(kTag,
             "ISR_PUBLISH_TRACE_BEGIN count=%u capacity=%u total=%u first=%u worst=%u overflow=%u frame_not_ready=%u",
             static_cast<unsigned>(retained_count),
             static_cast<unsigned>(kIsrPublishTraceCapacity),
             static_cast<unsigned>(s_isr_publish_trace_total),
             static_cast<unsigned>(s_isr_publish_trace_first_count),
             static_cast<unsigned>(s_isr_publish_trace_worst_count),
             s_isr_publish_trace_overflow ? 1u : 0u,
             static_cast<unsigned>(s_frame_not_ready_count.load(std::memory_order_relaxed)));
    for (size_t i = 0; i < s_isr_publish_trace_first_count; ++i) {
        LogIsrPublishTraceRecord(s_isr_publish_trace[i], "FIRST");
    }
    for (size_t i = 0; i < s_isr_publish_trace_worst_count; ++i) {
        LogIsrPublishTraceRecord(
            s_isr_publish_trace[kIsrPublishTraceFirstCapacity + i], "WORST");
    }
    ESP_LOGI(kTag, "ISR_PUBLISH_TRACE_END");
}
#endif

#if FACTORY_MARKER_GAP_RUNTIME_DIAGNOSTICS
uint64_t RuntimeCounterDelta(configRUN_TIME_COUNTER_TYPE before,
                             configRUN_TIME_COUNTER_TYPE after) {
    if (after >= before) {
        return static_cast<uint64_t>(after - before);
    }
    return static_cast<uint64_t>(
               std::numeric_limits<configRUN_TIME_COUNTER_TYPE>::max() - before) +
           1ULL + static_cast<uint64_t>(after);
}

void RefreshRuntimeTaskInventory() {
    constexpr UBaseType_t kScanCapacity = 48;
    TaskStatus_t task_status[kScanCapacity]{};
    configRUN_TIME_COUNTER_TYPE total_runtime = 0;

    const UBaseType_t found =
        uxTaskGetSystemState(task_status, kScanCapacity, &total_runtime);
    if (found == 0) {
        s_runtime_inventory_ready.store(false);
        ESP_LOGW(kTag,
                 "Marker-gap runtime inventory failed; task array may be too small");
        return;
    }

    size_t stored = 0;
    for (UBaseType_t i = 0; i < found && stored < kMaxRuntimeTasks; ++i) {
        // ESP-IDF 6.0 exposes TaskStatus_t::xCoreID only when the optional
        // task-list CoreID field is enabled. Query affinity through the
        // supported API instead so runtime diagnostics do not depend on that
        // formatting/configuration option.
        const BaseType_t core_id = xTaskGetCoreID(task_status[i].xHandle);
        if (core_id != 0 && core_id != tskNO_AFFINITY) {
            continue;
        }
        if (task_status[i].uxCurrentPriority < kRuntimeDiagnosticMinPriority) {
            continue;
        }

        RuntimeTaskRef& ref = s_runtime_tasks[stored++];
        ref.handle = task_status[i].xHandle;
        ref.core_id = core_id;
        ref.priority = task_status[i].uxCurrentPriority;
        std::snprintf(ref.name, sizeof(ref.name), "%s", task_status[i].pcTaskName);
    }

    s_runtime_task_count = stored;
    s_runtime_inventory_ready.store(stored > 0);
    ESP_LOGI(kTag,
             "Marker-gap runtime inventory ready: core0_or_unpinned_tasks=%u total_tasks=%u",
             static_cast<unsigned>(stored),
             static_cast<unsigned>(found));
}

void CaptureRuntimeSnapshot(RuntimeSnapshot* snapshot) {
    if (snapshot == nullptr) {
        return;
    }

    snapshot->valid = false;
    snapshot->count = 0;
    snapshot->capture_begin_us = 0;
    snapshot->captured_us = 0;

    if (!s_runtime_inventory_ready.load()) {
        return;
    }

    snapshot->capture_begin_us = esp_timer_get_time();
    const size_t count = std::min(s_runtime_task_count, kMaxRuntimeTasks);
    for (size_t i = 0; i < count; ++i) {
        TaskStatus_t task_status{};
        vTaskGetInfo(s_runtime_tasks[i].handle,
                     &task_status,
                     pdFALSE,
                     eRunning);
        snapshot->counters[i] = task_status.ulRunTimeCounter;
    }

    snapshot->count = count;
    snapshot->captured_us = esp_timer_get_time();
    snapshot->valid = true;
}

template <typename DiagnosticT>
void InsertRuntimeDelta(DiagnosticT* diagnostic,
                        const RuntimeTaskRef& ref,
                        uint64_t runtime_delta_us) {
    if (diagnostic == nullptr || runtime_delta_us == 0) {
        return;
    }

    size_t pos = diagnostic->delta_count;
    if (pos < kMaxReportedRuntimeDeltas) {
        ++diagnostic->delta_count;
    } else if (runtime_delta_us <=
               diagnostic->deltas[kMaxReportedRuntimeDeltas - 1].runtime_delta_us) {
        return;
    } else {
        pos = kMaxReportedRuntimeDeltas - 1;
    }

    while (pos > 0 &&
           runtime_delta_us > diagnostic->deltas[pos - 1].runtime_delta_us) {
        if (pos < kMaxReportedRuntimeDeltas) {
            diagnostic->deltas[pos] = diagnostic->deltas[pos - 1];
        }
        --pos;
    }

    RuntimeTaskDelta& out = diagnostic->deltas[pos];
    std::snprintf(out.name, sizeof(out.name), "%s", ref.name);
    out.core_id = ref.core_id;
    out.priority = ref.priority;
    out.runtime_delta_us = runtime_delta_us;
}

void QueueMarkerGapDiagnostic(uint32_t boundary,
                              int64_t target_local_us,
                              int64_t flip_commit_us,
                              int64_t toggle_before_us,
                              int64_t toggle_after_us,
                              const RuntimeSnapshot& previous_snapshot,
                              const RuntimeSnapshot& current_snapshot) {
    const int64_t total_marker_gap_us = toggle_after_us - flip_commit_us;
    if (total_marker_gap_us <= kMarkerGapThresholdUs) {
        return;
    }
    if (s_marker_gap_diagnostic_count >= kMarkerGapDiagnosticCapacity) {
        s_marker_gap_diagnostic_overflow = true;
        return;
    }

    MarkerGapDiagnostic& diagnostic =
        s_marker_gap_diagnostics[s_marker_gap_diagnostic_count++];
    diagnostic.boundary = boundary;
    diagnostic.target_local_us = target_local_us;
    diagnostic.flip_commit_us = flip_commit_us;
    diagnostic.toggle_before_us = toggle_before_us;
    diagnostic.toggle_after_us = toggle_after_us;
    diagnostic.stats_before_us =
        previous_snapshot.valid ? previous_snapshot.captured_us : 0;
    diagnostic.stats_after_us =
        current_snapshot.valid ? current_snapshot.captured_us : 0;
    diagnostic.diag_queue_drops = 0;

    if (previous_snapshot.valid && current_snapshot.valid) {
        const size_t count = std::min(previous_snapshot.count, current_snapshot.count);
        for (size_t i = 0; i < count; ++i) {
            const uint64_t delta = RuntimeCounterDelta(previous_snapshot.counters[i],
                                                       current_snapshot.counters[i]);
            InsertRuntimeDelta(&diagnostic, s_runtime_tasks[i], delta);
        }
    }
}

void RecordCommitDelayDiagnostic(uint32_t boundary,
                                 int64_t target_local_us,
                                 const PrecisionWindowTrace& precision,
                                 int64_t flip_request_us,
                                 int64_t flip_commit_us,
                                 int64_t toggle_after_us,
                                 const RuntimeSnapshot& runtime_after) {
    if ((flip_commit_us - target_local_us) <= kCommitDelayThresholdUs) {
        return;
    }
    if (s_commit_delay_diagnostic_count >= kCommitDelayDiagnosticCapacity) {
        s_commit_delay_diagnostic_overflow = true;
        return;
    }

    CommitDelayDiagnostic& out =
        s_commit_delay_diagnostics[s_commit_delay_diagnostic_count++];
    out.boundary = boundary;
    out.target_local_us = target_local_us;
    out.precision_entry_us = precision.entry_us;
    out.runtime_before_begin_us = precision.runtime_before.capture_begin_us;
    out.runtime_before_end_us = precision.runtime_before.captured_us;
    out.flip_request_us = flip_request_us;
    out.flip_commit_us = flip_commit_us;
    out.toggle_after_us = toggle_after_us;
    out.runtime_after_begin_us = runtime_after.capture_begin_us;
    out.runtime_after_end_us = runtime_after.captured_us;
    out.runtime_valid = precision.runtime_before.valid && runtime_after.valid;

    if (out.runtime_valid) {
        const size_t count = std::min(precision.runtime_before.count, runtime_after.count);
        for (size_t i = 0; i < count; ++i) {
            const uint64_t delta = RuntimeCounterDelta(
                precision.runtime_before.counters[i], runtime_after.counters[i]);
            InsertRuntimeDelta(&out, s_runtime_tasks[i], delta);
        }
    }
}

void DumpCommitDelayDiagnostics() {
    ESP_LOGI(kTag,
             "COMMIT_DELAY_TRACE_BEGIN count=%u capacity=%u overflow=%u threshold_us=%lld",
             static_cast<unsigned>(s_commit_delay_diagnostic_count),
             static_cast<unsigned>(kCommitDelayDiagnosticCapacity),
             s_commit_delay_diagnostic_overflow ? 1u : 0u,
             static_cast<long long>(kCommitDelayThresholdUs));

    for (size_t i = 0; i < s_commit_delay_diagnostic_count; ++i) {
        const CommitDelayDiagnostic& d = s_commit_delay_diagnostics[i];
        const int64_t wake_lateness_us = d.precision_entry_us - d.target_local_us;
        const int64_t snapshot_cost_us =
            (d.runtime_before_begin_us > 0 && d.runtime_before_end_us > 0)
                ? d.runtime_before_end_us - d.runtime_before_begin_us
                : -1;
        const int64_t request_lateness_us = d.flip_request_us - d.target_local_us;
        const int64_t flip_call_us = d.flip_commit_us - d.flip_request_us;
        const int64_t commit_lateness_us = d.flip_commit_us - d.target_local_us;
        const int64_t runtime_window_us =
            (d.runtime_before_end_us > 0 && d.runtime_after_begin_us > 0)
                ? d.runtime_after_begin_us - d.runtime_before_end_us
                : -1;

        ESP_LOGW(kTag,
                 "COMMIT_DELAY boundary=%u target_local_us=%lld precision_entry_us=%lld wake_lateness_us=%lld runtime_snapshot_cost_us=%lld flip_request_us=%lld request_lateness_us=%lld flip_commit_us=%lld flip_call_us=%lld commit_lateness_us=%lld toggle_after_us=%lld runtime_window_us=%lld runtime_valid=%u",
                 static_cast<unsigned>(d.boundary),
                 static_cast<long long>(d.target_local_us),
                 static_cast<long long>(d.precision_entry_us),
                 static_cast<long long>(wake_lateness_us),
                 static_cast<long long>(snapshot_cost_us),
                 static_cast<long long>(d.flip_request_us),
                 static_cast<long long>(request_lateness_us),
                 static_cast<long long>(d.flip_commit_us),
                 static_cast<long long>(flip_call_us),
                 static_cast<long long>(commit_lateness_us),
                 static_cast<long long>(d.toggle_after_us),
                 static_cast<long long>(runtime_window_us),
                 d.runtime_valid ? 1u : 0u);

        for (uint8_t rank = 0; rank < d.delta_count; ++rank) {
            const RuntimeTaskDelta& task = d.deltas[rank];
            ESP_LOGW(kTag,
                     "COMMIT_DELAY_TASK boundary=%u rank=%u task=%s core=%d priority=%u runtime_delta_us=%llu",
                     static_cast<unsigned>(d.boundary),
                     static_cast<unsigned>(rank + 1),
                     task.name,
                     static_cast<int>(task.core_id),
                     static_cast<unsigned>(task.priority),
                     static_cast<unsigned long long>(task.runtime_delta_us));
        }
    }
    ESP_LOGI(kTag, "COMMIT_DELAY_TRACE_END");
}

void DumpMarkerGapDiagnostics() {
    ESP_LOGI(kTag,
             "MARKER_GAP_TRACE_BEGIN count=%u capacity=%u overflow=%u threshold_us=%lld",
             static_cast<unsigned>(s_marker_gap_diagnostic_count),
             static_cast<unsigned>(kMarkerGapDiagnosticCapacity),
             s_marker_gap_diagnostic_overflow ? 1u : 0u,
             static_cast<long long>(kMarkerGapThresholdUs));
    for (size_t index = 0; index < s_marker_gap_diagnostic_count; ++index) {
        const MarkerGapDiagnostic& diagnostic = s_marker_gap_diagnostics[index];
        const int64_t flip_to_before_us =
            diagnostic.toggle_before_us - diagnostic.flip_commit_us;
        const int64_t toggle_call_us =
            diagnostic.toggle_after_us - diagnostic.toggle_before_us;
        const int64_t flip_to_after_us =
            diagnostic.toggle_after_us - diagnostic.flip_commit_us;
        const int64_t runtime_interval_us =
            (diagnostic.stats_before_us > 0 && diagnostic.stats_after_us > 0)
                ? diagnostic.stats_after_us - diagnostic.stats_before_us
                : -1;

        ESP_LOGW(kTag,
                 "MARKER_GAP boundary=%u target_local_us=%lld flip_commit_us=%lld toggle_before_us=%lld toggle_after_us=%lld flip_to_before_us=%lld toggle_call_us=%lld flip_to_after_us=%lld runtime_interval_us=%lld",
                 static_cast<unsigned>(diagnostic.boundary),
                 static_cast<long long>(diagnostic.target_local_us),
                 static_cast<long long>(diagnostic.flip_commit_us),
                 static_cast<long long>(diagnostic.toggle_before_us),
                 static_cast<long long>(diagnostic.toggle_after_us),
                 static_cast<long long>(flip_to_before_us),
                 static_cast<long long>(toggle_call_us),
                 static_cast<long long>(flip_to_after_us),
                 static_cast<long long>(runtime_interval_us));

        for (uint8_t i = 0; i < diagnostic.delta_count; ++i) {
            const RuntimeTaskDelta& task = diagnostic.deltas[i];
            ESP_LOGW(kTag,
                     "MARKER_GAP_TASK boundary=%u rank=%u task=%s core=%d priority=%u runtime_delta_us=%llu",
                     static_cast<unsigned>(diagnostic.boundary),
                     static_cast<unsigned>(i + 1),
                     task.name,
                     static_cast<int>(task.core_id),
                     static_cast<unsigned>(task.priority),
                     static_cast<unsigned long long>(task.runtime_delta_us));
        }
    }
    ESP_LOGI(kTag, "MARKER_GAP_TRACE_END");
}

#else
void RefreshRuntimeTaskInventory() {}
void CaptureRuntimeSnapshot(RuntimeSnapshot*) {}
void RecordCommitDelayDiagnostic(uint32_t, int64_t, const PrecisionWindowTrace&, int64_t, int64_t, int64_t, const RuntimeSnapshot&) {}
void DumpCommitDelayDiagnostics() {}
void DumpMarkerGapDiagnostics() {}
void QueueMarkerGapDiagnostic(uint32_t,
                              int64_t,
                              int64_t,
                              int64_t,
                              int64_t,
                              const RuntimeSnapshot&,
                              const RuntimeSnapshot&) {}
#endif

void ResetBoundaryTrace() {
    s_boundary_trace_count = 0;
    s_boundary_trace_overflow = false;
#if FACTORY_MARKER_GAP_RUNTIME_DIAGNOSTICS
    s_commit_delay_diagnostic_count = 0;
    s_commit_delay_diagnostic_overflow = false;
    s_marker_gap_diagnostic_count = 0;
    s_marker_gap_diagnostic_overflow = false;
#endif
}

void RecordBoundaryTrace(uint32_t boundary,
                         int64_t boundary_disciplined_us,
                         int64_t wait_local_deadline_us,
                         const PrecisionWindowTrace& precision,
                         int64_t flip_request_us,
                         int64_t flip_commit_us,
                         int64_t toggle_after_us,
                         int64_t post_local_deadline_us) {
    if (s_boundary_trace_count >= kBoundaryTraceCapacity) {
        s_boundary_trace_overflow = true;
        return;
    }

    BoundaryTraceRecord& out = s_boundary_trace[s_boundary_trace_count++];
    out.boundary = boundary;
    out.boundary_disciplined_us = boundary_disciplined_us;
    out.wait_local_deadline_us = wait_local_deadline_us;
    out.precision_entry_us = precision.entry_us;
    out.runtime_snapshot_before_begin_us = precision.runtime_before.capture_begin_us;
    out.runtime_snapshot_before_end_us = precision.runtime_before.captured_us;
    out.flip_request_us = flip_request_us;
    out.flip_commit_us = flip_commit_us;
    out.toggle_after_us = toggle_after_us;
    out.post_local_deadline_us = post_local_deadline_us;
}

void DumpBoundaryTrace() {
    ESP_LOGI(kTag,
             "BOUNDARY_TRACE_BEGIN count=%u capacity=%u overflow=%u",
             static_cast<unsigned>(s_boundary_trace_count),
             static_cast<unsigned>(kBoundaryTraceCapacity),
             s_boundary_trace_overflow ? 1u : 0u);

    int64_t previous_wait_deadline_us = 0;
    for (size_t i = 0; i < s_boundary_trace_count; ++i) {
        const BoundaryTraceRecord& r = s_boundary_trace[i];
        const int64_t step_us =
            previous_wait_deadline_us == 0
                ? 0
                : r.wait_local_deadline_us - previous_wait_deadline_us;
        const int64_t precision_entry_lateness_us =
            r.precision_entry_us - r.wait_local_deadline_us;
        const int64_t runtime_snapshot_cost_us =
            (r.runtime_snapshot_before_begin_us > 0 &&
             r.runtime_snapshot_before_end_us > 0)
                ? r.runtime_snapshot_before_end_us - r.runtime_snapshot_before_begin_us
                : -1;
        const int64_t flip_request_lateness_wait_us =
            r.flip_request_us - r.wait_local_deadline_us;
        const int64_t flip_call_us = r.flip_commit_us - r.flip_request_us;
        const int64_t flip_lateness_wait_us =
            r.flip_commit_us - r.wait_local_deadline_us;
        const int64_t toggle_lateness_wait_us =
            r.toggle_after_us - r.wait_local_deadline_us;
        const int64_t post_minus_wait_us =
            r.post_local_deadline_us - r.wait_local_deadline_us;
        const int64_t flip_lateness_post_us =
            r.flip_commit_us - r.post_local_deadline_us;

        ESP_LOGI(kTag,
                 "BOUNDARY_TRACE boundary=%u disciplined_us=%lld boundary_local_us=%lld step_us=%lld precision_entry_us=%lld precision_entry_lateness_us=%lld runtime_snapshot_cost_us=%lld flip_request_us=%lld flip_request_lateness_us=%lld flip_commit_us=%lld flip_call_us=%lld toggle_after_us=%lld post_local_us=%lld post_minus_wait_us=%lld flip_lateness_wait_us=%lld toggle_lateness_wait_us=%lld flip_lateness_post_us=%lld",
                 static_cast<unsigned>(r.boundary),
                 static_cast<long long>(r.boundary_disciplined_us),
                 static_cast<long long>(r.wait_local_deadline_us),
                 static_cast<long long>(step_us),
                 static_cast<long long>(r.precision_entry_us),
                 static_cast<long long>(precision_entry_lateness_us),
                 static_cast<long long>(runtime_snapshot_cost_us),
                 static_cast<long long>(r.flip_request_us),
                 static_cast<long long>(flip_request_lateness_wait_us),
                 static_cast<long long>(r.flip_commit_us),
                 static_cast<long long>(flip_call_us),
                 static_cast<long long>(r.toggle_after_us),
                 static_cast<long long>(r.post_local_deadline_us),
                 static_cast<long long>(post_minus_wait_us),
                 static_cast<long long>(flip_lateness_wait_us),
                 static_cast<long long>(toggle_lateness_wait_us),
                 static_cast<long long>(flip_lateness_post_us));

        previous_wait_deadline_us = r.wait_local_deadline_us;
    }

    ESP_LOGI(kTag, "BOUNDARY_TRACE_END");
}

// Seven-segment mask bits: A B C D E F G.
constexpr uint8_t kA = 1u << 0;
constexpr uint8_t kB = 1u << 1;
constexpr uint8_t kC = 1u << 2;
constexpr uint8_t kD = 1u << 3;
constexpr uint8_t kE = 1u << 4;
constexpr uint8_t kF = 1u << 5;
constexpr uint8_t kG = 1u << 6;

constexpr uint8_t kDigitSegments[10] = {
    kA | kB | kC | kD | kE | kF,           // 0
    kB | kC,                                 // 1
    kA | kB | kD | kE | kG,                 // 2
    kA | kB | kC | kD | kG,                 // 3
    kB | kC | kF | kG,                      // 4
    kA | kC | kD | kF | kG,                 // 5
    kA | kC | kD | kE | kF | kG,            // 6
    kA | kB | kC,                            // 7
    kA | kB | kC | kD | kE | kF | kG,       // 8
    kA | kB | kC | kD | kF | kG,            // 9
};

void Fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    factory_display_backend_fill_rect(x, y, w, h, r, g, b);
}

void DrawDigitMmSs(int origin_x, uint8_t digit, uint8_t r, uint8_t g, uint8_t b) {
    if (digit > 9) return;

    // Compact 11x30 seven-segment digit. Four digits plus a colon fit in 64x32.
    constexpr int thickness = 3;
    constexpr int horizontal_length = 7;
    constexpr int right_x = 8;
    constexpr int top_y = 1;
    constexpr int upper_vertical_y = 4;
    constexpr int upper_vertical_height = 9;
    constexpr int middle_y = 14;
    constexpr int lower_vertical_y = 18;
    constexpr int lower_vertical_height = 9;
    constexpr int bottom_y = 28;

    const uint8_t mask = kDigitSegments[digit];

    // Digits 1, 4 and 7 have fewer horizontal segments than the rounded
    // seven-segment shapes, so the normal geometry makes them look shorter.
    // Extend their vertical strokes to the same y=1..29 visual height while
    // preserving the compact MM:SS center gap.
    if (digit == 1) {
        Fill(origin_x + right_x, 1, thickness, 12, r, g, b);   // y=1..12
        Fill(origin_x + right_x, 18, thickness, 12, r, g, b);  // y=18..29
        return;
    }

    if (digit == 4) {
        Fill(origin_x, 1, thickness, 12, r, g, b);             // F: y=1..12
        Fill(origin_x + right_x, 1, thickness, 12, r, g, b);  // B: y=1..12
        Fill(origin_x + 2, middle_y, horizontal_length, thickness, r, g, b);
        Fill(origin_x + right_x, 18, thickness, 12, r, g, b); // C: y=18..29
        return;
    }

    if (digit == 7) {
        Fill(origin_x + 2, top_y, horizontal_length, thickness, r, g, b);
        Fill(origin_x + right_x, 1, thickness, 12, r, g, b);  // B: y=1..12
        Fill(origin_x + right_x, 18, thickness, 12, r, g, b); // C: y=18..29
        return;
    }

    if (mask & kA) Fill(origin_x + 2, top_y, horizontal_length, thickness, r, g, b);
    if (mask & kB) Fill(origin_x + right_x, upper_vertical_y, thickness,
                        upper_vertical_height, r, g, b);
    if (mask & kC) Fill(origin_x + right_x, lower_vertical_y, thickness,
                        lower_vertical_height, r, g, b);
    if (mask & kD) Fill(origin_x + 2, bottom_y, horizontal_length, thickness, r, g, b);
    if (mask & kE) Fill(origin_x, lower_vertical_y, thickness,
                        lower_vertical_height, r, g, b);
    if (mask & kF) Fill(origin_x, upper_vertical_y, thickness,
                        upper_vertical_height, r, g, b);
    if (mask & kG) Fill(origin_x + 2, middle_y, horizontal_length, thickness, r, g, b);
}

void DrawColon(uint8_t r, uint8_t g, uint8_t b) {
    // Centered between minute and second pairs.
    Fill(30, 9, 3, 3, r, g, b);
    Fill(30, 20, 3, 3, r, g, b);
}

void RenderMinutesSeconds(uint32_t total_seconds, uint8_t r, uint8_t g, uint8_t b) {
    total_seconds = std::min<uint32_t>(total_seconds, kMaxDisplayedSeconds);
    const uint32_t minutes = total_seconds / 60u;
    const uint32_t seconds = total_seconds % 60u;

    factory_display_backend_clear();

    // 1..11, 14..24, colon 30..32, 37..47, 50..60.
    DrawDigitMmSs(1,  static_cast<uint8_t>((minutes / 10u) % 10u), r, g, b);
    DrawDigitMmSs(14, static_cast<uint8_t>(minutes % 10u), r, g, b);
    DrawColon(r, g, b);
    DrawDigitMmSs(37, static_cast<uint8_t>((seconds / 10u) % 10u), r, g, b);
    DrawDigitMmSs(50, static_cast<uint8_t>(seconds % 10u), r, g, b);
}

void RenderIdleToBackBuffer() {
    // Full-panel 64x32 RGB888 logo. The source bitmap is stored as 0xRRGGBB.
    // Classic ESP32 quantizes each channel to its existing 3-bit BCM format;
    // ESP32-S3 keeps the existing HUB75 driver's configured color depth.
    factory_display_backend_clear();
    for (int y = 0; y < kIdleLogoHeight; ++y) {
        for (int x = 0; x < kIdleLogoWidth; ++x) {
            const uint32_t color =
                kIdleLogoBitmap[y * kIdleLogoWidth + x];
            const uint8_t r = static_cast<uint8_t>((color >> 16) & 0xFFu);
            const uint8_t g = static_cast<uint8_t>((color >> 8) & 0xFFu);
            const uint8_t b = static_cast<uint8_t>(color & 0xFFu);
            factory_display_backend_set_pixel(x, y, r, g, b);
        }
    }
}

void RenderRunningToBackBuffer(uint32_t remaining_seconds) {
    RenderMinutesSeconds(remaining_seconds, 255, 0, 0);
}

void RenderFinishedToBackBuffer() {
    RenderMinutesSeconds(0, 255, 0, 0);
}

bool TryReceiveReplacement(DisplayCommand* replacement) {
    return xQueueReceive(s_command_queue, replacement, 0) == pdTRUE;
}

// Wait until a local esp_timer deadline, but let RESET/re-arm preempt the wait.
// This task is lower priority than both the deadline TimerTask and CommandTask.
// It shares CPU0 with control/network work so the classic ESP32 software scan
// can own CPU1 without once-per-second drawing preemptions. It never performs
// drawing in the countdown TimerTask itself.
bool WaitUntilOrReplacement(int64_t deadline_us, DisplayCommand* replacement) {
    while (true) {
        if (TryReceiveReplacement(replacement)) return false;

        const int64_t now_us = esp_timer_get_time();
        const int64_t remaining_us = deadline_us - now_us;
        if (remaining_us <= 0) return true;

        if (remaining_us > kFineLeadUs) {
            const int64_t sleep_budget_us = remaining_us - kFineLeadUs;
            const TickType_t ticks = static_cast<TickType_t>(sleep_budget_us / kTickUs);
            if (ticks > 0) {
                if (xQueueReceive(s_command_queue, replacement, ticks) == pdTRUE) {
                    return false;
                }
                continue;
            }
        }

        // Final short window. Higher-priority TimerTask/CommandTask can still
        // preempt this spin, so timing-critical firmware behavior remains first.
        while (esp_timer_get_time() < deadline_us) {
            if (TryReceiveReplacement(replacement)) return false;
        }
        return true;
    }
}

// Wait for a deadline expressed in the continuous DS3231-disciplined domain.
// Re-resolve it into esp_timer time after each coarse sleep so an anchor/reslope
// update can adjust the next visible second without introducing a phase jump.
bool WaitUntilDisciplinedOrReplacement(int64_t disciplined_deadline_us,
                                        DisplayCommand* replacement,
                                        int64_t* resolved_local_deadline_us,
                                        PrecisionWindowTrace* precision_trace) {
    while (true) {
        if (TryReceiveReplacement(replacement)) return false;

        const int64_t local_deadline_us =
            rtc_discipline_disciplined_to_local_us(disciplined_deadline_us);
        if (resolved_local_deadline_us != nullptr) {
            *resolved_local_deadline_us = local_deadline_us;
        }

        const int64_t now_us = esp_timer_get_time();
        const int64_t remaining_us = local_deadline_us - now_us;
        if (remaining_us <= 0) return true;

        if (remaining_us > kFineLeadUs) {
            const int64_t sleep_budget_us = remaining_us - kFineLeadUs;
            const TickType_t ticks = static_cast<TickType_t>(sleep_budget_us / kTickUs);
            if (ticks > 0) {
                if (xQueueReceive(s_command_queue, replacement, ticks) == pdTRUE) {
                    return false;
                }
                continue;
            }
        }

        if (precision_trace != nullptr) {
            precision_trace->entry_us = esp_timer_get_time();
            const int64_t margin_us = local_deadline_us - precision_trace->entry_us;
            if (margin_us >= kRuntimeSnapshotMinimumMarginUs) {
                CaptureRuntimeSnapshot(&precision_trace->runtime_before);
            }
        }

        /*
         * Freeze the already re-resolved local deadline for the final <=2 ms
         * spin. The old loop called disciplined_to_local on every iteration,
         * which enters the discipline critical section each time. A rate update
         * inside this tiny tail can move the pending deadline only by a tiny
         * fraction of a microsecond, while repeated critical sections can add
         * measurable scheduler contention on classic ESP32 core 0.
         */
        while (esp_timer_get_time() < local_deadline_us) {
            if (TryReceiveReplacement(replacement)) return false;
        }
        return true;
    }
}

bool ReceiveStartAnchor(uint64_t command_id,
                        int64_t fallback_start_disciplined_us,
                        int64_t* start_disciplined_us) {
    if (start_disciplined_us == nullptr) return false;

    // The TimerTask owns the actual Armed -> Running transition. On the S3 the
    // display task runs on the other core and can reach the first flip slightly
    // before that task publishes its epoch, so allow a short bounded wait after
    // the visible START. This does not move the first frame deadline.
    DisplayStartAnchor anchor{};
    if (s_start_anchor_queue != nullptr) {
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (xQueueReceive(s_start_anchor_queue, &anchor,
                              pdMS_TO_TICKS(10)) != pdTRUE) {
                continue;
            }
            if (anchor.command_id == command_id &&
                anchor.start_disciplined_us > 0) {
                *start_disciplined_us = anchor.start_disciplined_us;
                return true;
            }
            // A superseded command can race with a new arm on the other core.
            // Discard a stale anchor and continue waiting for the current one.
        }
    }

    *start_disciplined_us = fallback_start_disciplined_us;
    ESP_LOGW(kTag,
             "Disciplined START anchor unavailable/mismatched for command=%016llX; using local-deadline conversion",
             static_cast<unsigned long long>(command_id));
    return false;
}

void DisplayTask(void*) {
    DisplayCommand command{};

    while (true) {
        if (xQueueReceive(s_command_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // New commands supersede an older visual schedule. Drain to newest.
        DisplayCommand newest{};
        while (TryReceiveReplacement(&newest)) command = newest;

        if (command.type == DisplayCommand::Type::Reset) {
#if CONFIG_IDF_TARGET_ESP32
            CancelBoundaryPublishTimer();
#endif
            SetPresentationDiagnosticEdge(false);
            RenderIdleToBackBuffer();
            factory_display_backend_flip();
            continue;
        }

        if (command.duration_seconds > kMaxDisplayedSeconds) {
            ESP_LOGW(kTag,
                     "MM:SS display supports up to 99:59; duration=%u will show 99:59 until remaining <=5999",
                     static_cast<unsigned>(command.duration_seconds));
        }

        // Re-arm the presentation diagnostic low before this countdown.
#if CONFIG_IDF_TARGET_ESP32
        CancelBoundaryPublishTimer();
#endif
        SetPresentationDiagnosticEdge(false);
        ResetBoundaryTrace();
#if CONFIG_IDF_TARGET_ESP32
        // v6.15 includes START in the same ISR publication trace as all later
        // one-second boundaries. This also resets frame-not-ready accounting
        // before the prepared START frame is armed.
        ResetIsrPublishTrace();
#endif

        // Make the idle/device ID frame visible while armed, then pre-render
        // the first countdown frame. Nothing visible changes until deadline.
        RenderIdleToBackBuffer();
        factory_display_backend_flip();
        RenderRunningToBackBuffer(command.duration_seconds);

        // Refresh the task inventory once per armed run, several seconds
        // before START. The expensive system-state enumeration therefore never
        // runs in the precision window.
        RefreshRuntimeTaskInventory();

        DisplayCommand replacement{};

        // START uses the exact same single-index ISR publication path as the
        // subsequent one-second boundaries. The master/TimerTask START path is
        // unchanged; this timer controls only when the already-rendered first
        // display frame becomes visible.
        int64_t start_disciplined_us = 0;
        const int64_t fallback_start_disciplined_us =
            rtc_discipline_local_to_disciplined_us(command.local_start_us);
        int64_t commit_lateness_us = 0;
        int64_t worst_flip_lateness_us = 0;
        bool superseded = false;

#if CONFIG_IDF_TARGET_ESP32
        const uint32_t start_sequence = MakePublishSequence(0);
        MarkPreparedFrame(start_sequence);
        int64_t start_arm_us = 0;
        if (!ArmBoundaryPublish(0,
                                start_sequence,
                                fallback_start_disciplined_us,
                                command.local_start_us,
                                &start_arm_us)) {
            ESP_LOGE(kTag, "START display ISR timer arm failed; falling back to task publication");
            if (!WaitUntilOrReplacement(command.local_start_us, &replacement)) {
                command = replacement;
                xQueueOverwrite(s_command_queue, &command);
                continue;
            }
            const int64_t begin_us = esp_timer_get_time();
            factory_display_backend_flip();
            TogglePresentationDiagnosticEdge();
            const int64_t end_us = esp_timer_get_time();
            IsrPublishTraceRecord fallback{};
            fallback.boundary = 0;
            fallback.sequence = start_sequence;
            fallback.disciplined_us = fallback_start_disciplined_us;
            fallback.target_local_us = command.local_start_us;
            fallback.arm_us = start_arm_us;
            fallback.callback_entry_us = begin_us;
            fallback.publish_marker_begin_us = begin_us;
            fallback.publish_marker_end_us = end_us;
            fallback.callback_exit_us = end_us;
            fallback.post_local_us = command.local_start_us;
            fallback.prepared_sequence = start_sequence;
            fallback.published_sequence = start_sequence;
            fallback.marker_level = static_cast<uint8_t>(
                s_presentation_diagnostic_level.load(std::memory_order_relaxed));
            fallback.published = true;
            RetainIsrPublishTraceRecord(fallback);
            commit_lateness_us = end_us - command.local_start_us;
        } else {
            if (!WaitForBoundaryPublishOrReplacement(start_sequence, &replacement)) {
                command = replacement;
                xQueueOverwrite(s_command_queue, &command);
                continue;
            }
            const int64_t start_post_local_us =
                rtc_discipline_disciplined_to_local_us(fallback_start_disciplined_us);
            RecordIsrPublishTrace(0,
                                  start_sequence,
                                  fallback_start_disciplined_us,
                                  command.local_start_us,
                                  start_arm_us,
                                  start_post_local_us);
            const IsrPublishTraceRecord start_sample = s_isr_last_sample;
            commit_lateness_us = start_sample.published
                ? start_sample.publish_marker_end_us - command.local_start_us
                : start_sample.callback_exit_us - command.local_start_us;
        }
        s_prepared_sequence.store(0, std::memory_order_release);

        ReceiveStartAnchor(command.command_id,
                           fallback_start_disciplined_us,
                           &start_disciplined_us);

        ESP_LOGI(kTag,
                 "Visual countdown started: duration=%u target_local_us=%lld start_isr_publish_lateness_us=%lld frame_not_ready=%u",
                 static_cast<unsigned>(command.duration_seconds),
                 static_cast<long long>(command.local_start_us),
                 static_cast<long long>(commit_lateness_us),
                 static_cast<unsigned>(s_frame_not_ready_count.load(std::memory_order_relaxed)));

        int64_t worst_boundary_publish_lateness_us = commit_lateness_us;

        // v6.15 classic-ESP32 path: START and each next frame are rendered
        // early, then an interrupt-dispatched esp_timer publishes one active
        // frame index at the raw-local deadline. No priority-12 precision spin
        // remains in the visible presentation path.
#else
        if (!WaitUntilOrReplacement(command.local_start_us, &replacement)) {
            command = replacement;
            xQueueOverwrite(s_command_queue, &command);
            continue;
        }
        const int64_t start_flip_request_us = esp_timer_get_time();
        factory_display_backend_flip();
        const int64_t start_flip_commit_us = esp_timer_get_time();
        const int64_t start_toggle_before_us = esp_timer_get_time();
        TogglePresentationDiagnosticEdge();
        const int64_t start_toggle_after_us = esp_timer_get_time();
        RuntimeSnapshot previous_runtime_snapshot{};
        CaptureRuntimeSnapshot(&previous_runtime_snapshot);
        RuntimeSnapshot no_previous_runtime_snapshot{};
        QueueMarkerGapDiagnostic(0, command.local_start_us, start_flip_commit_us,
                                 start_toggle_before_us, start_toggle_after_us,
                                 no_previous_runtime_snapshot, previous_runtime_snapshot);
        ReceiveStartAnchor(command.command_id, fallback_start_disciplined_us,
                           &start_disciplined_us);
        commit_lateness_us = start_flip_commit_us - command.local_start_us;
        worst_flip_lateness_us = commit_lateness_us;
        ESP_LOGI(kTag,
                 "Visual countdown started: duration=%u target_local_us=%lld flip_request_lateness_us=%lld flip_commit_lateness_us=%lld",
                 static_cast<unsigned>(command.duration_seconds),
                 static_cast<long long>(command.local_start_us),
                 static_cast<long long>(start_flip_request_us - command.local_start_us),
                 static_cast<long long>(commit_lateness_us));
#endif

#if CONFIG_IDF_TARGET_ESP32
        for (uint32_t elapsed = 1; elapsed <= command.duration_seconds; ++elapsed) {
            const uint32_t remaining = command.duration_seconds - elapsed;
            if (remaining == 0) {
                RenderFinishedToBackBuffer();
            } else {
                RenderRunningToBackBuffer(remaining);
            }

            const int64_t boundary_disciplined_us =
                start_disciplined_us + static_cast<int64_t>(elapsed) * 1000000LL;
            const int64_t boundary_local_us =
                rtc_discipline_disciplined_to_local_us(boundary_disciplined_us);
            const uint32_t sequence = MakePublishSequence(elapsed);
            MarkPreparedFrame(sequence);

            int64_t arm_us = 0;
            if (!ArmBoundaryPublish(elapsed,
                                    sequence,
                                    boundary_disciplined_us,
                                    boundary_local_us,
                                    &arm_us)) {
                // A timer-arm failure is exceptional. Keep the display usable,
                // but make it obvious in validation rather than silently
                // skipping a second.
                const int64_t fallback_begin_us = esp_timer_get_time();
                factory_display_backend_flip();
                TogglePresentationDiagnosticEdge();
                const int64_t fallback_end_us = esp_timer_get_time();
                IsrPublishTraceRecord fallback{};
                fallback.boundary = elapsed;
                fallback.sequence = sequence;
                fallback.disciplined_us = boundary_disciplined_us;
                fallback.target_local_us = boundary_local_us;
                fallback.arm_us = arm_us;
                fallback.callback_entry_us = fallback_begin_us;
                fallback.publish_marker_begin_us = fallback_begin_us;
                fallback.publish_marker_end_us = fallback_end_us;
                fallback.callback_exit_us = fallback_end_us;
                fallback.post_local_us =
                    rtc_discipline_disciplined_to_local_us(boundary_disciplined_us);
                fallback.prepared_sequence = sequence;
                fallback.published_sequence = sequence;
                fallback.marker_level = static_cast<uint8_t>(
                    s_presentation_diagnostic_level.load(std::memory_order_relaxed));
                fallback.published = true;
                RetainIsrPublishTraceRecord(fallback);
                worst_boundary_publish_lateness_us = std::max(
                    worst_boundary_publish_lateness_us, fallback_end_us - boundary_local_us);
                s_prepared_sequence.store(0, std::memory_order_release);
                continue;
            }

            if (!WaitForBoundaryPublishOrReplacement(sequence, &replacement)) {
                command = replacement;
                xQueueOverwrite(s_command_queue, &command);
                superseded = true;
                break;
            }

            const int64_t post_local_us =
                rtc_discipline_disciplined_to_local_us(boundary_disciplined_us);
            RecordIsrPublishTrace(elapsed,
                                  sequence,
                                  boundary_disciplined_us,
                                  boundary_local_us,
                                  arm_us,
                                  post_local_us);
            const IsrPublishTraceRecord sample = s_isr_last_sample;
            if (sample.published) {
                worst_boundary_publish_lateness_us = std::max(
                    worst_boundary_publish_lateness_us,
                    sample.publish_marker_end_us - boundary_local_us);
            }
            s_prepared_sequence.store(0, std::memory_order_release);
        }
#else
        for (uint32_t elapsed = 1; elapsed <= command.duration_seconds; ++elapsed) {
            const uint32_t remaining = command.duration_seconds - elapsed;
            if (remaining == 0) {
                RenderFinishedToBackBuffer();
            } else {
                RenderRunningToBackBuffer(remaining);
            }

            const int64_t boundary_disciplined_us =
                start_disciplined_us + static_cast<int64_t>(elapsed) * 1000000LL;
            int64_t boundary_local_us = 0;
            PrecisionWindowTrace precision_trace{};
            if (!WaitUntilDisciplinedOrReplacement(boundary_disciplined_us,
                                                    &replacement,
                                                    &boundary_local_us,
                                                    &precision_trace)) {
                command = replacement;
                xQueueOverwrite(s_command_queue, &command);
                superseded = true;
                break;
            }

            const int64_t flip_request_us = esp_timer_get_time();
            factory_display_backend_flip();
            const int64_t flip_commit_us = esp_timer_get_time();
            const int64_t toggle_before_us = esp_timer_get_time();
            TogglePresentationDiagnosticEdge();
            const int64_t toggle_after_us = esp_timer_get_time();

            RuntimeSnapshot current_runtime_snapshot{};
            CaptureRuntimeSnapshot(&current_runtime_snapshot);
            RecordCommitDelayDiagnostic(elapsed,
                                        boundary_local_us,
                                        precision_trace,
                                        flip_request_us,
                                        flip_commit_us,
                                        toggle_after_us,
                                        current_runtime_snapshot);
            QueueMarkerGapDiagnostic(elapsed,
                                     boundary_local_us,
                                     flip_commit_us,
                                     toggle_before_us,
                                     toggle_after_us,
                                     previous_runtime_snapshot,
                                     current_runtime_snapshot);
            previous_runtime_snapshot = current_runtime_snapshot;

            // Preserve the exact local deadline that the wait loop used before
            // asking the inverse mapping again. If the disciplined->local
            // conversion develops a transient offset, this pair tells us
            // directly whether the deadline itself moved.
            const int64_t wait_local_deadline_us = boundary_local_us;
            const int64_t post_local_deadline_us =
                rtc_discipline_disciplined_to_local_us(boundary_disciplined_us);
            RecordBoundaryTrace(elapsed,
                                boundary_disciplined_us,
                                wait_local_deadline_us,
                                precision_trace,
                                flip_request_us,
                                flip_commit_us,
                                toggle_after_us,
                                post_local_deadline_us);

            // Keep the pre-existing worst-lateness behavior unchanged for A/B
            // comparability: it references a fresh post-marker conversion.
            boundary_local_us = post_local_deadline_us;
            worst_flip_lateness_us = std::max(worst_flip_lateness_us,
                                               flip_commit_us - boundary_local_us);
        }

#endif
        if (!superseded) {
#if CONFIG_IDF_TARGET_ESP32
            ESP_LOGI(kTag,
                     "Visual countdown finished: duration=%u start_isr_publish_lateness_us=%lld worst_isr_publish_marker_lateness_us=%lld frame_not_ready=%u",
                     static_cast<unsigned>(command.duration_seconds),
                     static_cast<long long>(commit_lateness_us),
                     static_cast<long long>(worst_boundary_publish_lateness_us),
                     static_cast<unsigned>(s_frame_not_ready_count.load(std::memory_order_relaxed)));
#else
            ESP_LOGI(kTag,
                     "Visual countdown finished: duration=%u worst_flip_commit_lateness_us=%lld",
                     static_cast<unsigned>(command.duration_seconds),
                     static_cast<long long>(worst_flip_lateness_us));
#endif
            // Dump only after the run. No per-boundary UART logging occurs in
            // the precision path.
#if CONFIG_IDF_TARGET_ESP32
            DumpIsrPublishTrace();
#else
            DumpBoundaryTrace();
            DumpCommitDelayDiagnostics();
            DumpMarkerGapDiagnostics();
#endif
        }
    }
}


}  // namespace

extern "C" bool factory_display_init(const char* device_id, uint8_t brightness) {
    if (s_ready.load()) return true;

    // Device identity remains part of the protocol/configuration, but the idle
    // panel now shows the common logo instead of the numeric device ID.
    (void)device_id;

#if defined(CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS
    gpio_config_t presentation_gpio{};
    presentation_gpio.pin_bit_mask = 1ULL << CONFIG_FACTORY_DISPLAY_EDGE_GPIO;
    presentation_gpio.mode = GPIO_MODE_OUTPUT;
    presentation_gpio.pull_up_en = GPIO_PULLUP_DISABLE;
    presentation_gpio.pull_down_en = GPIO_PULLDOWN_DISABLE;
    presentation_gpio.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&presentation_gpio));
    SetPresentationDiagnosticEdge(false);
    ESP_LOGI(kTag, "Presentation-commit diagnostic enabled on GPIO%d",
             CONFIG_FACTORY_DISPLAY_EDGE_GPIO);
#endif

    if (!factory_display_backend_init(brightness)) {
        ESP_LOGE(kTag, "HUB75 backend initialization failed");
        return false;
    }

    s_command_queue = xQueueCreate(1, sizeof(DisplayCommand));
    s_start_anchor_queue = xQueueCreate(1, sizeof(DisplayStartAnchor));
    if (s_command_queue == nullptr || s_start_anchor_queue == nullptr) {
        ESP_LOGE(kTag, "Could not create display scheduler queues");
        return false;
    }

#if CONFIG_IDF_TARGET_ESP32
    esp_timer_create_args_t publish_timer_args{};
    publish_timer_args.callback = &BoundaryPublishTimerCallback;
    publish_timer_args.arg = nullptr;
    publish_timer_args.dispatch_method = ESP_TIMER_ISR;
    publish_timer_args.name = "display_publish";
    const esp_err_t publish_timer_result =
        esp_timer_create(&publish_timer_args, &s_boundary_publish_timer);
    if (publish_timer_result != ESP_OK) {
        ESP_LOGE(kTag, "Could not create ISR frame-publication timer: %s",
                 esp_err_to_name(publish_timer_result));
        return false;
    }
    ESP_LOGI(kTag,
             "ISR frame publication enabled: esp_timer_dispatch=ISR prepared_frame_sequence_guard=1 active_frame_publish=single_index");
#endif

#if FACTORY_MARKER_GAP_RUNTIME_DIAGNOSTICS
    ESP_LOGI(kTag,
             "Marker-gap diagnostics enabled: threshold_us=%lld runtime_stats=esp_timer buffered_until_run_end=1 commit_delay_runtime_snapshots=precision-entry-to-post-marker",
             static_cast<long long>(kMarkerGapThresholdUs));
    ESP_LOGI(kTag,
             "Inverse-mapping boundary trace enabled: capacity=%u buffered_until_run_end=1",
             static_cast<unsigned>(kBoundaryTraceCapacity));
#endif

    // Establish a known visible idle frame before the scheduler task starts.
    RenderIdleToBackBuffer();
    factory_display_backend_flip();

    if (xTaskCreatePinnedToCore(&DisplayTask, "countdown_display", 4096, nullptr,
                                kDisplaySchedulerPriority, &s_display_task,
                                kDisplaySchedulerCore) != pdPASS) {
        ESP_LOGE(kTag, "Could not create display scheduler task");
        return false;
    }

    s_ready.store(true);
#if CONFIG_IDF_TARGET_ESP32
    ESP_LOGI(kTag,
             "64x32 countdown display ready: idle=logo running=MM:SS scheduler_core=%d priority=%u second_boundaries=isr_publish",
             static_cast<int>(kDisplaySchedulerCore),
             static_cast<unsigned>(kDisplaySchedulerPriority));
#else
    ESP_LOGI(kTag,
             "64x32 countdown display ready: idle=logo running=MM:SS scheduler_core=%d priority=%u",
             static_cast<int>(kDisplaySchedulerCore),
             static_cast<unsigned>(kDisplaySchedulerPriority));
#endif
    return true;
}

extern "C" void factory_display_set_brightness_percent(uint8_t brightness_percent) {
    if (!s_ready.load()) return;
    if (brightness_percent > 100) brightness_percent = 100;
    factory_display_backend_set_brightness_percent(brightness_percent);
}

extern "C" void factory_display_arm(int64_t local_start_us,
                                      uint32_t duration_seconds,
                                      uint64_t command_id) {
    if (!s_ready.load() || s_command_queue == nullptr) return;
    if (s_start_anchor_queue != nullptr) {
        xQueueReset(s_start_anchor_queue);
    }
    DisplayCommand command{};
    command.type = DisplayCommand::Type::Arm;
    command.local_start_us = local_start_us;
    command.duration_seconds = duration_seconds;
    command.command_id = command_id;
    xQueueOverwrite(s_command_queue, &command);
#if CONFIG_IDF_TARGET_ESP32
    if (s_display_task != nullptr) xTaskNotifyGive(s_display_task);
#endif
}

extern "C" void factory_display_note_started(uint64_t command_id,
                                               int64_t start_disciplined_us) {
    if (!s_ready.load() || s_start_anchor_queue == nullptr ||
        command_id == 0 || start_disciplined_us <= 0) {
        return;
    }
    DisplayStartAnchor anchor{};
    anchor.command_id = command_id;
    anchor.start_disciplined_us = start_disciplined_us;
    xQueueOverwrite(s_start_anchor_queue, &anchor);
}

extern "C" void factory_display_reset(void) {
    if (!s_ready.load() || s_command_queue == nullptr) return;
    if (s_start_anchor_queue != nullptr) {
        xQueueReset(s_start_anchor_queue);
    }
    DisplayCommand command{};
    command.type = DisplayCommand::Type::Reset;
    xQueueOverwrite(s_command_queue, &command);
#if CONFIG_IDF_TARGET_ESP32
    if (s_display_task != nullptr) xTaskNotifyGive(s_display_task);
#endif
}
