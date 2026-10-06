#include "cpu0_latency_monitor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "cpu0_latency";

#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
constexpr uint32_t kPeriodUs = CONFIG_FACTORY_CPU0_LATENCY_PERIOD_US;
constexpr uint32_t kThresholdUs = CONFIG_FACTORY_CPU0_LATENCY_THRESHOLD_US;
constexpr size_t kEventCapacity = 64;
constexpr size_t kCommitEventCapacity = 32;
constexpr size_t kTaskInventoryCapacity = 48;
constexpr int kCommitInterruptLevel = CONFIG_ESP_TIMER_INTERRUPT_LEVEL;
#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
constexpr uint32_t kTaskCanaryHoldUs = CONFIG_FACTORY_CPU0_LATENCY_CANARY_HOLD_US;
#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND
// Kconfig intentionally hides the boundary/lead symbols in mid-second mode, so
// do not reference those CONFIG_* macros here. Zero is reported for boundary
// and lead in the validation-only serial record.
constexpr uint32_t kTaskCanaryBoundary = 0;
constexpr int64_t kTaskCanaryLeadUs = 0;
constexpr int64_t kTaskCanaryAfterRunUs = CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_AFTER_RUN_US;
#else
constexpr uint32_t kTaskCanaryBoundary = CONFIG_FACTORY_CPU0_LATENCY_CANARY_BOUNDARY;
constexpr int64_t kTaskCanaryLeadUs = CONFIG_FACTORY_CPU0_LATENCY_CANARY_LEAD_US;
#endif
#endif
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
constexpr int64_t kIsrCanaryOffsetUs = CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY_AFTER_RUN_US;
constexpr uint32_t kIsrCanaryHoldUs = CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY_HOLD_US;
#endif
#if defined(CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT) && CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT
constexpr char kHeartbeatTraffic[] = "suppressed";
#else
constexpr char kHeartbeatTraffic[] = "enabled";
#endif
static_assert(kPeriodUs >= 100, "Latency monitor period must be >=100 us");
static_assert(kThresholdUs < kPeriodUs * 8U, "Latency threshold is unexpectedly large");
static_assert(kCommitInterruptLevel >= 1 && kCommitInterruptLevel <= 3,
              "Commit esp_timer interrupt level must be 1..3");
#if !defined(CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0) || !CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0
#error "CPU0 latency monitor requires CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0=y"
#endif
#if !defined(CONFIG_GPTIMER_ISR_CACHE_SAFE) || !CONFIG_GPTIMER_ISR_CACHE_SAFE
#error "CPU0 latency monitor requires CONFIG_GPTIMER_ISR_CACHE_SAFE=y"
#endif
#if !defined(CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM) || !CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM
#error "CPU0 latency monitor requires CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM=y"
#endif
#if !defined(CONFIG_GPTIMER_OBJ_CACHE_SAFE) || !CONFIG_GPTIMER_OBJ_CACHE_SAFE
#error "CPU0 latency monitor requires CONFIG_GPTIMER_OBJ_CACHE_SAFE=y"
#endif
#if !defined(CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM) || !CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM
#error "CPU0 latency monitor requires CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM=y"
#endif

struct LatencyEvent {
    uint32_t sequence{};
    uint32_t lateness_us{};
    int64_t expected_local_us{};
    int64_t actual_local_us{};
    TaskHandle_t interrupted_task{};
};

struct CommitLateEvent {
    uint32_t sequence{};
    uint32_t boundary{};
    int64_t target_local_us{};
    int64_t callback_entry_us{};
    int64_t marker_begin_us{};
    int64_t marker_end_us{};
    TaskHandle_t interrupted_task{};
};

struct TaskIdentity {
    TaskHandle_t handle{};
    char name[configMAX_TASK_NAME_LEN]{};
    BaseType_t core_id{tskNO_AFFINITY};
    UBaseType_t priority{};
};

#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
struct TaskCanaryRecord {
    bool requested{};
    bool completed{};
    uint32_t boundary{};
    int64_t target_local_us{};
    int64_t critical_begin_us{};
    int64_t critical_end_us{};
    uint32_t hold_us{};
};
#endif

#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
struct IsrCanaryRecord {
    bool requested{};
    bool completed{};
    int64_t requested_after_begin_us{};
    int64_t isr_begin_us{};
    int64_t isr_end_us{};
    uint32_t hold_us{};
    TaskHandle_t interrupted_task{};
};
#endif

DRAM_ATTR gptimer_handle_t s_timer = nullptr;
DRAM_ATTR gptimer_alarm_config_t s_next_alarm_config{};
DRAM_ATTR volatile bool s_initialized = false;
DRAM_ATTR volatile bool s_run_active = false;
DRAM_ATTR volatile uint64_t s_run_command_id = 0;
DRAM_ATTR volatile int64_t s_commit_target_local_us = 0;
DRAM_ATTR volatile uint32_t s_commit_target_boundary = 0;
DRAM_ATTR volatile int64_t s_run_begin_local_us = 0;
DRAM_ATTR volatile int64_t s_timer_to_local_offset_us = 0;
DRAM_ATTR volatile bool s_timer_local_mapping_valid = false;
DRAM_ATTR volatile uint32_t s_sample_callbacks = 0;
DRAM_ATTR volatile uint32_t s_missed_periods = 0;
DRAM_ATTR volatile uint32_t s_rearm_failures = 0;
DRAM_ATTR volatile uint32_t s_wrong_core_callbacks = 0;

DRAM_ATTR volatile uint32_t s_event_sequence = 0;
DRAM_ATTR volatile uint32_t s_event_total_count = 0;
DRAM_ATTR volatile uint32_t s_event_retained_count = 0;
DRAM_ATTR volatile uint32_t s_event_overflow = 0;
DRAM_ATTR LatencyEvent s_events[kEventCapacity]{};
DRAM_ATTR LatencyEvent s_last_event{};
DRAM_ATTR volatile bool s_last_event_valid = false;
DRAM_ATTR volatile uint32_t s_worst_event_lateness_us = 0;
DRAM_ATTR volatile uint32_t s_worst_event_sequence = 0;
DRAM_ATTR TaskHandle_t s_worst_event_task = nullptr;

DRAM_ATTR volatile uint32_t s_commit_event_sequence = 0;
DRAM_ATTR volatile uint32_t s_commit_total_count = 0;
DRAM_ATTR volatile uint32_t s_commit_retained_count = 0;
DRAM_ATTR volatile uint32_t s_commit_event_overflow = 0;
DRAM_ATTR CommitLateEvent s_commit_events[kCommitEventCapacity]{};
DRAM_ATTR CommitLateEvent s_last_commit{};
DRAM_ATTR volatile bool s_last_commit_valid = false;
DRAM_ATTR volatile uint32_t s_worst_commit_lateness_us = 0;

DRAM_ATTR volatile bool s_overlap_found = false;
DRAM_ATTR volatile uint32_t s_overlap_sample_lateness_us = 0;
DRAM_ATTR volatile uint32_t s_overlap_commit_lateness_us = 0;
DRAM_ATTR TaskHandle_t s_overlap_task = nullptr;

#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
DRAM_ATTR portMUX_TYPE s_task_canary_mux = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR TaskCanaryRecord s_task_canary{};
TaskHandle_t s_task_canary_task = nullptr;
#endif

#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
DRAM_ATTR esp_timer_handle_t s_isr_canary_timer = nullptr;
DRAM_ATTR IsrCanaryRecord s_isr_canary{};
#endif

TaskIdentity s_task_inventory[kTaskInventoryCapacity]{};
size_t s_task_inventory_count = 0;
cpu0_latency_monitor_summary_t s_summary{};
portMUX_TYPE s_summary_mux = portMUX_INITIALIZER_UNLOCKED;

void CopyTaskName(char* destination, std::size_t capacity, const char* source) {
    if (destination == nullptr || capacity == 0) return;
    const char* text = source != nullptr ? source : "UNKNOWN";
    std::size_t out = 0;
    while (text[out] != '\0' && out + 1 < capacity) {
        const char c = text[out];
        destination[out] = (c == '|' || c == ',' || c == '\r' || c == '\n') ? '_' : c;
        ++out;
    }
    destination[out] = '\0';
}

void RefreshTaskInventory() {
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY
    TaskStatus_t status[kTaskInventoryCapacity]{};
    configRUN_TIME_COUNTER_TYPE total_runtime = 0;
    const UBaseType_t found = uxTaskGetSystemState(status,
                                                   kTaskInventoryCapacity,
                                                   &total_runtime);
    s_task_inventory_count = 0;
    for (UBaseType_t i = 0; i < found && s_task_inventory_count < kTaskInventoryCapacity; ++i) {
        TaskIdentity& out = s_task_inventory[s_task_inventory_count++];
        out.handle = status[i].xHandle;
        out.core_id = xTaskGetCoreID(status[i].xHandle);
        out.priority = status[i].uxCurrentPriority;
        std::snprintf(out.name, sizeof(out.name), "%s", status[i].pcTaskName);
    }
#else
    s_task_inventory_count = 0;
#endif
}

const TaskIdentity* FindTaskIdentity(TaskHandle_t handle) {
    for (size_t i = 0; i < s_task_inventory_count; ++i) {
        if (s_task_inventory[i].handle == handle) return &s_task_inventory[i];
    }
    return nullptr;
}

bool IRAM_ATTR EventCouldOverlapCommit(const LatencyEvent& event, int64_t commit_target_local_us) {
    // The retained event proves that the sampler deadline at expected_local_us
    // could not execute until actual_local_us. The continuous blocker can have
    // started any time after the previous 250 us sampler deadline, so use one
    // sample period of backward uncertainty. This is intentionally called a
    // plausible overlap, not proof of exact blocker start time.
    const int64_t earliest_possible_block_us =
        event.expected_local_us - static_cast<int64_t>(kPeriodUs);
    return commit_target_local_us >= earliest_possible_block_us &&
           commit_target_local_us <= event.actual_local_us;
}

void IRAM_ATTR NoteOverlap(const LatencyEvent& event,
                           uint32_t commit_lateness_us) {
    if (!s_overlap_found || commit_lateness_us > s_overlap_commit_lateness_us) {
        s_overlap_found = true;
        s_overlap_sample_lateness_us = event.lateness_us;
        s_overlap_commit_lateness_us = commit_lateness_us;
        s_overlap_task = event.interrupted_task;
    }
}

bool IRAM_ATTR OnAlarm(gptimer_handle_t timer,
                       const gptimer_alarm_event_data_t* event_data,
                       void*) {
    if (event_data == nullptr) {
        s_rearm_failures = s_rearm_failures + 1U;
        return false;
    }

    // v6.23.15: the free-running 1 MHz GPTimer is authoritative for
    // sampler lateness. With auto-reload disabled, alarm_value is the exact
    // deadline that first became overdue and count_value is the timer count
    // on ISR entry. No software expected-time state participates in the
    // subtraction, so a delayed callback cannot silently discard one period.
    const uint64_t count_value = event_data->count_value;
    const uint64_t alarm_value = event_data->alarm_value;
    const uint64_t late_ticks = count_value >= alarm_value
        ? (count_value - alarm_value)
        : 0ULL;
    const uint64_t skipped_periods64 =
        late_ticks / static_cast<uint64_t>(kPeriodUs);
    const uint64_t next_alarm_count = alarm_value +
        (skipped_periods64 + 1ULL) * static_cast<uint64_t>(kPeriodUs);

    // Keep the sampling grid anchored to the original absolute GPTimer alarm
    // sequence. If ISR work consumes the remaining margin and the new alarm is
    // already in the past, the GPTimer driver triggers it immediately rather
    // than moving the grid. This preserves the missed-period accounting.
    // CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM makes gptimer_set_alarm_action()
    // callable while flash cache is disabled. Keep the config object itself in
    // DRAM as required by the driver cache-safety contract; only alarm_count
    // changes on each callback.
    s_next_alarm_config.alarm_count = next_alarm_count;
    if (gptimer_set_alarm_action(timer, &s_next_alarm_config) != ESP_OK) {
        s_rearm_failures = s_rearm_failures + 1U;
        return false;
    }

    if (!s_run_active) return false;
    s_sample_callbacks = s_sample_callbacks + 1U;

    const uint32_t skipped_periods = skipped_periods64 > static_cast<uint64_t>(UINT32_MAX)
        ? UINT32_MAX
        : static_cast<uint32_t>(skipped_periods64);
    if (UINT32_MAX - s_missed_periods < skipped_periods) {
        s_missed_periods = UINT32_MAX;
    } else {
        s_missed_periods = s_missed_periods + skipped_periods;
    }
    if (xPortGetCoreID() != 0) {
        s_wrong_core_callbacks = s_wrong_core_callbacks + 1U;
    }

    const uint32_t lateness_us = late_ticks > static_cast<uint64_t>(UINT32_MAX)
        ? UINT32_MAX
        : static_cast<uint32_t>(late_ticks);
    if (lateness_us < kThresholdUs) return false;

    // Store only the raw TCB handle in the ISR. Name/core/priority resolution
    // is deferred until post-run. The local timestamps are derived from the
    // GPTimer count using a run-start calibration, so the cache-safe sampler
    // path does not call esp_timer_get_time(). The authoritative lateness value
    // remains count_value - alarm_value and is independent of this mapping.
    const int64_t timer_to_local = s_timer_to_local_offset_us;
    const int64_t expected_local_us = s_timer_local_mapping_valid
        ? static_cast<int64_t>(alarm_value) + timer_to_local
        : 0;
    const int64_t actual_local_us = s_timer_local_mapping_valid
        ? static_cast<int64_t>(count_value) + timer_to_local
        : 0;
    const TaskHandle_t interrupted = xTaskGetCurrentTaskHandleForCore(0);
    const uint32_t sequence = s_event_sequence + 1U;
    s_event_sequence = sequence;
    s_event_total_count = s_event_total_count + 1U;

    LatencyEvent event{};
    event.sequence = sequence;
    event.lateness_us = lateness_us;
    event.actual_local_us = actual_local_us;
    event.expected_local_us = expected_local_us;
    event.interrupted_task = interrupted;
    s_last_event = event;
    s_last_event_valid = true;

    if (lateness_us > s_worst_event_lateness_us) {
        s_worst_event_lateness_us = lateness_us;
        s_worst_event_sequence = sequence;
        s_worst_event_task = interrupted;
    }

    const uint32_t retained = s_event_retained_count;
    if (retained < kEventCapacity) {
        s_events[retained] = event;
        s_event_retained_count = retained + 1U;
    } else {
        s_event_overflow = s_event_overflow + 1U;
    }

    if (s_timer_local_mapping_valid && s_last_commit_valid &&
        EventCouldOverlapCommit(event, s_last_commit.target_local_us)) {
        const int64_t commit_late64 =
            s_last_commit.callback_entry_us - s_last_commit.target_local_us;
        const uint32_t commit_lateness_us = commit_late64 <= 0
            ? 0U
            : (commit_late64 > static_cast<int64_t>(UINT32_MAX)
                ? UINT32_MAX
                : static_cast<uint32_t>(commit_late64));
        NoteOverlap(event, commit_lateness_us);
    }

    return false;
}

#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
void IRAM_ATTR IsrCanaryCallback(void*) {
    if (!s_run_active) return;
    s_isr_canary.isr_begin_us = esp_timer_get_time();
    s_isr_canary.interrupted_task = xTaskGetCurrentTaskHandleForCore(0);
    esp_rom_delay_us(kIsrCanaryHoldUs);
    s_isr_canary.isr_end_us = esp_timer_get_time();
    s_isr_canary.completed = true;
}
#endif

#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
void TaskCanaryTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_run_active) continue;

        s_task_canary = TaskCanaryRecord{};
        s_task_canary.requested = true;
        s_task_canary.hold_us = kTaskCanaryHoldUs;

#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND
        s_task_canary.boundary = 0;
        const int64_t begin_target_us = s_run_begin_local_us + kTaskCanaryAfterRunUs;
        s_task_canary.target_local_us = begin_target_us;
        while (s_run_active) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t remaining_us = begin_target_us - now_us;
            if (remaining_us <= 0) break;
            if (remaining_us > 3000) {
                const TickType_t ticks = pdMS_TO_TICKS(
                    static_cast<uint32_t>((remaining_us - 2000) / 1000));
                if (ticks > 0) {
                    vTaskDelay(ticks);
                    continue;
                }
            }
            while (s_run_active && esp_timer_get_time() < begin_target_us) {
                // Validation-only precision wait immediately before the target.
            }
            break;
        }
        if (!s_run_active) continue;
#else
        s_task_canary.boundary = kTaskCanaryBoundary;
        while (s_run_active) {
            const uint32_t boundary = s_commit_target_boundary;
            const int64_t target_local_us = s_commit_target_local_us;
            if (boundary < kTaskCanaryBoundary || target_local_us <= 0) {
                // Block for at least one RTOS tick. CONFIG_FREERTOS_HZ is 100
                // in the classic-ESP32 fleet profiles, so pdMS_TO_TICKS(1) is 0.
                vTaskDelay(1);
                continue;
            }
            if (boundary > kTaskCanaryBoundary) break;

            const int64_t begin_target_us = target_local_us - kTaskCanaryLeadUs;
            s_task_canary.target_local_us = target_local_us;
            for (;;) {
                if (!s_run_active || s_commit_target_boundary != kTaskCanaryBoundary) break;
                const int64_t now_us = esp_timer_get_time();
                const int64_t remaining_us = begin_target_us - now_us;
                if (remaining_us <= 0) break;
                if (remaining_us > 3000) {
                    const TickType_t ticks = pdMS_TO_TICKS(
                        static_cast<uint32_t>((remaining_us - 2000) / 1000));
                    if (ticks > 0) {
                        vTaskDelay(ticks);
                        continue;
                    }
                }
                while (s_run_active &&
                       s_commit_target_boundary == kTaskCanaryBoundary &&
                       esp_timer_get_time() < begin_target_us) {
                    // Validation-only precision wait immediately before the target.
                }
                break;
            }
            if (!s_run_active || s_commit_target_boundary != kTaskCanaryBoundary) break;
            break;
        }
        if (!s_run_active || s_commit_target_boundary != kTaskCanaryBoundary) continue;
#endif

        portENTER_CRITICAL(&s_task_canary_mux);
        s_task_canary.critical_begin_us = esp_timer_get_time();
        esp_rom_delay_us(kTaskCanaryHoldUs);
        s_task_canary.critical_end_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_task_canary_mux);
        s_task_canary.completed = true;
    }
}

#endif

void BuildSummary() {
    cpu0_latency_monitor_summary_t summary{};
    summary.valid = (s_rearm_failures == 0 && s_timer_local_mapping_valid);
    summary.command_id = s_run_command_id;
    summary.sample_callbacks = s_sample_callbacks;
    summary.missed_periods = s_missed_periods;
    summary.event_count = s_event_total_count;
    summary.worst_lateness_us = s_worst_event_lateness_us;
    summary.commit_late_count = s_commit_total_count;
    summary.worst_commit_lateness_us = s_worst_commit_lateness_us;
    summary.commit_overlap = s_overlap_found;
    summary.overlap_sample_lateness_us = s_overlap_sample_lateness_us;
    summary.overlap_commit_lateness_us = s_overlap_commit_lateness_us;
    summary.wrong_core_callbacks = s_wrong_core_callbacks;
    summary.event_overflow = s_event_overflow;
    summary.commit_overflow = s_commit_event_overflow;
    summary.sampler_intr_level = static_cast<uint8_t>(kCommitInterruptLevel);
    summary.commit_intr_level = static_cast<uint8_t>(kCommitInterruptLevel);

    const TaskIdentity* worst = FindTaskIdentity(s_worst_event_task);
    CopyTaskName(summary.worst_task, sizeof(summary.worst_task),
                 s_event_total_count == 0 ? "NONE" : (worst ? worst->name : "UNKNOWN"));
    const TaskIdentity* overlap = FindTaskIdentity(s_overlap_task);
    CopyTaskName(summary.overlap_task, sizeof(summary.overlap_task),
                 !s_overlap_found ? "NONE" : (overlap ? overlap->name : "UNKNOWN"));

    portENTER_CRITICAL(&s_summary_mux);
    s_summary = summary;
    portEXIT_CRITICAL(&s_summary_mux);
}

#endif  // CONFIG_FACTORY_CPU0_LATENCY_MONITOR

}  // namespace

bool cpu0_latency_monitor_init() {
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    if (s_initialized) return true;
    if (xPortGetCoreID() != 0) {
        ESP_LOGE(kTag, "Latency monitor init must run on CPU0; core=%d", xPortGetCoreID());
        return false;
    }

    gptimer_config_t config{};
    config.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    config.direction = GPTIMER_COUNT_UP;
    config.resolution_hz = 1000000;
    config.intr_priority = kCommitInterruptLevel;
    if (gptimer_new_timer(&config, &s_timer) != ESP_OK) return false;

    gptimer_event_callbacks_t callbacks{};
    callbacks.on_alarm = &OnAlarm;
    if (gptimer_register_event_callbacks(s_timer, &callbacks, nullptr) != ESP_OK) return false;
    if (gptimer_enable(s_timer) != ESP_OK) return false;
    if (gptimer_set_raw_count(s_timer, 0) != ESP_OK) return false;

    s_next_alarm_config = {};
    s_next_alarm_config.reload_count = 0;
    s_next_alarm_config.flags.auto_reload_on_alarm = false;

    gptimer_alarm_config_t alarm{};
    alarm.alarm_count = kPeriodUs;
    alarm.reload_count = 0;
    alarm.flags.auto_reload_on_alarm = false;
    if (gptimer_set_alarm_action(s_timer, &alarm) != ESP_OK) return false;
    if (gptimer_start(s_timer) != ESP_OK) return false;

#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
    if (xTaskCreatePinnedToCore(&TaskCanaryTask,
                                "lat_canary",
                                2048,
                                nullptr,
                                configMAX_PRIORITIES - 1,
                                &s_task_canary_task,
                                0) != pdPASS) {
        return false;
    }
#endif
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
    esp_timer_create_args_t isr_canary_args{};
    isr_canary_args.callback = &IsrCanaryCallback;
    isr_canary_args.dispatch_method = ESP_TIMER_ISR;
    isr_canary_args.name = "lat_isr_canary";
    if (esp_timer_create(&isr_canary_args, &s_isr_canary_timer) != ESP_OK) return false;
#endif

    s_initialized = true;
    ESP_LOGI(kTag,
             "CPU0 fleet latency monitor ready: period_us=%u threshold_us=%u sampler_intr_level=%d commit_intr_level=%d core=0 task_canary=%u isr_canary=%u gpio_probe=off",
             static_cast<unsigned>(kPeriodUs),
             static_cast<unsigned>(kThresholdUs),
             kCommitInterruptLevel,
             kCommitInterruptLevel,
#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
             1U,
#else
             0U,
#endif
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
             1U);
#else
             0U);
#endif
    return true;
#else
    return true;
#endif
}

void cpu0_latency_monitor_prepare_run(uint64_t command_id) {
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    if (!s_initialized) return;
    s_run_active = false;
    s_run_command_id = command_id;
    s_commit_target_local_us = 0;
    s_commit_target_boundary = 0;
    s_run_begin_local_us = 0;
    s_timer_to_local_offset_us = 0;
    s_timer_local_mapping_valid = false;
    s_event_sequence = 0;
    s_event_total_count = 0;
    s_event_retained_count = 0;
    s_event_overflow = 0;
    s_sample_callbacks = 0;
    s_missed_periods = 0;
    s_rearm_failures = 0;
    s_wrong_core_callbacks = 0;
    s_worst_event_lateness_us = 0;
    s_worst_event_sequence = 0;
    s_worst_event_task = nullptr;
    s_last_event = LatencyEvent{};
    s_last_event_valid = false;
    s_commit_event_sequence = 0;
    s_commit_total_count = 0;
    s_commit_retained_count = 0;
    s_commit_event_overflow = 0;
    s_worst_commit_lateness_us = 0;
    s_last_commit = CommitLateEvent{};
    s_last_commit_valid = false;
    s_overlap_found = false;
    s_overlap_sample_lateness_us = 0;
    s_overlap_commit_lateness_us = 0;
    s_overlap_task = nullptr;
#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
    s_task_canary = TaskCanaryRecord{};
#endif
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
    s_isr_canary = IsrCanaryRecord{};
    if (s_isr_canary_timer != nullptr) (void)esp_timer_stop(s_isr_canary_timer);
#endif
    std::memset(s_events, 0, sizeof(s_events));
    std::memset(s_commit_events, 0, sizeof(s_commit_events));
    portENTER_CRITICAL(&s_summary_mux);
    s_summary = {};
    s_summary.command_id = command_id;
    portEXIT_CRITICAL(&s_summary_mux);
    RefreshTaskInventory();
    ESP_LOGI(kTag,
             "CPU0_LATENCY_RUN_PREP command=%016llX task_inventory=%u task_canary=%u isr_canary=%u gpio_probe=off",
             static_cast<unsigned long long>(command_id),
             static_cast<unsigned>(s_task_inventory_count),
#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
             1U,
#else
             0U,
#endif
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
             1U);
#else
             0U);
#endif
#else
    (void)command_id;
#endif
}

void cpu0_latency_monitor_begin_run(uint64_t command_id, int64_t start_disciplined_us) {
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    if (!s_initialized) return;
    s_run_command_id = command_id;

    // Calibrate the free-running GPTimer count into the esp_timer local domain
    // before enabling run accounting. Use the midpoint of two esp_timer reads
    // around the raw-count read to bound the mapping error to the read span.
    uint64_t timer_count = 0;
    const int64_t local_before_us = esp_timer_get_time();
    const esp_err_t count_err = gptimer_get_raw_count(s_timer, &timer_count);
    const int64_t local_after_us = esp_timer_get_time();
    if (count_err == ESP_OK) {
        const int64_t local_mid_us = local_before_us +
            ((local_after_us - local_before_us) / 2);
        s_timer_to_local_offset_us =
            local_mid_us - static_cast<int64_t>(timer_count);
        s_timer_local_mapping_valid = true;
        s_run_begin_local_us = local_mid_us;
    } else {
        s_timer_to_local_offset_us = 0;
        s_timer_local_mapping_valid = false;
        s_run_begin_local_us = local_after_us;
        s_rearm_failures = s_rearm_failures + 1U;
    }
    s_run_active = true;
#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
    if (s_task_canary_task != nullptr) xTaskNotifyGive(s_task_canary_task);
#endif
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
    if (s_isr_canary_timer != nullptr) {
        (void)esp_timer_stop(s_isr_canary_timer);
        s_isr_canary.requested = true;
        s_isr_canary.requested_after_begin_us = kIsrCanaryOffsetUs;
        s_isr_canary.hold_us = kIsrCanaryHoldUs;
        if (esp_timer_start_once(s_isr_canary_timer, kIsrCanaryOffsetUs) != ESP_OK) {
            s_isr_canary.requested = false;
        }
    }
#endif
    ESP_LOGI(kTag,
             "CPU0_LATENCY_RUN_BEGIN command=%016llX start_disciplined_us=%lld task_inventory=%u heartbeat_traffic=%s task_canary=%u isr_canary=%u gpio_probe=off",
             static_cast<unsigned long long>(command_id),
             static_cast<long long>(start_disciplined_us),
             static_cast<unsigned>(s_task_inventory_count),
             kHeartbeatTraffic,
#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
             1U,
#else
             0U,
#endif
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
             1U);
#else
             0U);
#endif
#else
    (void)command_id;
    (void)start_disciplined_us;
#endif
}

void cpu0_latency_monitor_end_run() {
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    s_run_active = false;
    s_commit_target_local_us = 0;
    s_commit_target_boundary = 0;
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
    if (s_isr_canary_timer != nullptr) (void)esp_timer_stop(s_isr_canary_timer);
#endif
    BuildSummary();
#endif
}

void cpu0_latency_monitor_set_commit_target(uint32_t boundary, int64_t target_local_us) {
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    s_commit_target_boundary = boundary;
    s_commit_target_local_us = target_local_us;
#else
    (void)boundary;
    (void)target_local_us;
#endif
}

void IRAM_ATTR cpu0_latency_monitor_note_commit(uint32_t boundary,
                                                int64_t target_local_us,
                                                int64_t callback_entry_us,
                                                int64_t marker_begin_us,
                                                int64_t marker_end_us) {
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    const int64_t lateness64 = callback_entry_us - target_local_us;
    if (!s_run_active || lateness64 < static_cast<int64_t>(kThresholdUs)) return;

    const uint32_t lateness_us = lateness64 > static_cast<int64_t>(UINT32_MAX)
        ? UINT32_MAX
        : static_cast<uint32_t>(lateness64);
    const TaskHandle_t interrupted = xTaskGetCurrentTaskHandleForCore(0);
    const uint32_t sequence = s_commit_event_sequence + 1U;
    s_commit_event_sequence = sequence;
    s_commit_total_count = s_commit_total_count + 1U;
    if (lateness_us > s_worst_commit_lateness_us) {
        s_worst_commit_lateness_us = lateness_us;
    }

    CommitLateEvent event{};
    event.sequence = sequence;
    event.boundary = boundary;
    event.target_local_us = target_local_us;
    event.callback_entry_us = callback_entry_us;
    event.marker_begin_us = marker_begin_us;
    event.marker_end_us = marker_end_us;
    event.interrupted_task = interrupted;
    s_last_commit.sequence = event.sequence;
    s_last_commit.boundary = event.boundary;
    s_last_commit.target_local_us = event.target_local_us;
    s_last_commit.callback_entry_us = event.callback_entry_us;
    s_last_commit.marker_begin_us = event.marker_begin_us;
    s_last_commit.marker_end_us = event.marker_end_us;
    s_last_commit.interrupted_task = event.interrupted_task;
    s_last_commit_valid = true;

    const uint32_t retained = s_commit_retained_count;
    if (retained < kCommitEventCapacity) {
        CommitLateEvent& stored = s_commit_events[retained];
        stored.sequence = event.sequence;
        stored.boundary = event.boundary;
        stored.target_local_us = event.target_local_us;
        stored.callback_entry_us = event.callback_entry_us;
        stored.marker_begin_us = event.marker_begin_us;
        stored.marker_end_us = event.marker_end_us;
        stored.interrupted_task = event.interrupted_task;
        s_commit_retained_count = retained + 1U;
    } else {
        s_commit_event_overflow = s_commit_event_overflow + 1U;
    }

    if (s_timer_local_mapping_valid && s_last_event_valid && EventCouldOverlapCommit(s_last_event, target_local_us)) {
        NoteOverlap(s_last_event, lateness_us);
    }
#else
    (void)boundary;
    (void)target_local_us;
    (void)callback_entry_us;
    (void)marker_begin_us;
    (void)marker_end_us;
#endif
}

void cpu0_latency_monitor_dump_run() {
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    cpu0_latency_monitor_summary_t summary{};
    cpu0_latency_monitor_get_summary(&summary);
    ESP_LOGI(kTag,
             "CPU0_FLEET_SUMMARY command=%016llX valid=%u period_us=%u threshold_us=%u sample_callbacks=%u missed_periods=%u events=%u worst_us=%u worst_task=%s commit_late_events=%u worst_commit_us=%u overlap=%u overlap_sample_us=%u overlap_commit_us=%u overlap_task=%s wrong_core_callbacks=%u event_overflow=%u commit_overflow=%u rearm_failures=%u sampler_intr_level=%u commit_intr_level=%u task_canary=%u isr_canary=%u gpio_probe=off",
             static_cast<unsigned long long>(summary.command_id),
             summary.valid ? 1U : 0U,
             static_cast<unsigned>(kPeriodUs),
             static_cast<unsigned>(kThresholdUs),
             static_cast<unsigned>(summary.sample_callbacks),
             static_cast<unsigned>(summary.missed_periods),
             static_cast<unsigned>(summary.event_count),
             static_cast<unsigned>(summary.worst_lateness_us),
             summary.worst_task,
             static_cast<unsigned>(summary.commit_late_count),
             static_cast<unsigned>(summary.worst_commit_lateness_us),
             summary.commit_overlap ? 1U : 0U,
             static_cast<unsigned>(summary.overlap_sample_lateness_us),
             static_cast<unsigned>(summary.overlap_commit_lateness_us),
             summary.overlap_task,
             static_cast<unsigned>(summary.wrong_core_callbacks),
             static_cast<unsigned>(summary.event_overflow),
             static_cast<unsigned>(summary.commit_overflow),
             static_cast<unsigned>(s_rearm_failures),
             static_cast<unsigned>(summary.sampler_intr_level),
             static_cast<unsigned>(summary.commit_intr_level),
#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
             1U,
#else
             0U,
#endif
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
             1U);
#else
             0U);
#endif

    // Preserve concise serial post-mortem detail when a board is connected,
    // while fleet operation relies only on the STATUS summary fields.
#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY
    ESP_LOGI(kTag,
             "CPU0_LATENCY_CANARY requested=%u completed=%u mode=%s boundary=%u target_local_us=%lld critical_begin_us=%lld critical_end_us=%lld duration_us=%lld lead_us=%lld hold_us=%u task=lat_canary",
             s_task_canary.requested ? 1U : 0U,
             s_task_canary.completed ? 1U : 0U,
#if defined(CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND) && CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND
             "midsecond",
#else
             "boundary",
#endif
             static_cast<unsigned>(s_task_canary.boundary),
             static_cast<long long>(s_task_canary.target_local_us),
             static_cast<long long>(s_task_canary.critical_begin_us),
             static_cast<long long>(s_task_canary.critical_end_us),
             static_cast<long long>(s_task_canary.critical_end_us - s_task_canary.critical_begin_us),
             static_cast<long long>(kTaskCanaryLeadUs),
             static_cast<unsigned>(s_task_canary.hold_us));
#endif
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
    const TaskIdentity* isr_task = FindTaskIdentity(s_isr_canary.interrupted_task);
    ESP_LOGI(kTag,
             "CPU0_ISR_CANARY requested=%u completed=%u requested_after_begin_us=%lld isr_begin_us=%lld isr_end_us=%lld duration_us=%lld hold_us=%u interrupted_task=%s",
             s_isr_canary.requested ? 1U : 0U,
             s_isr_canary.completed ? 1U : 0U,
             static_cast<long long>(s_isr_canary.requested_after_begin_us),
             static_cast<long long>(s_isr_canary.isr_begin_us),
             static_cast<long long>(s_isr_canary.isr_end_us),
             static_cast<long long>(s_isr_canary.isr_end_us - s_isr_canary.isr_begin_us),
             static_cast<unsigned>(s_isr_canary.hold_us),
             isr_task ? isr_task->name : "UNKNOWN");
#endif

    for (uint32_t i = 0; i < s_event_retained_count && i < kEventCapacity; ++i) {
        const LatencyEvent& e = s_events[i];
        const TaskIdentity* task = FindTaskIdentity(e.interrupted_task);
        ESP_LOGW(kTag,
                 "CPU0_LATENCY_EVENT seq=%u lateness_us=%u expected_local_us=%lld actual_local_us=%lld interrupted_task=%s core=%d priority=%u",
                 static_cast<unsigned>(e.sequence),
                 static_cast<unsigned>(e.lateness_us),
                 static_cast<long long>(e.expected_local_us),
                 static_cast<long long>(e.actual_local_us),
                 task ? task->name : "UNKNOWN",
                 task ? static_cast<int>(task->core_id) : -2,
                 task ? static_cast<unsigned>(task->priority) : 0U);
    }

    for (uint32_t i = 0; i < s_commit_retained_count && i < kCommitEventCapacity; ++i) {
        const CommitLateEvent& e = s_commit_events[i];
        const TaskIdentity* task = FindTaskIdentity(e.interrupted_task);
        ESP_LOGW(kTag,
                 "CPU0_COMMIT_LATE seq=%u boundary=%u target_local_us=%lld callback_entry_us=%lld callback_lateness_us=%lld marker_begin_us=%lld marker_end_us=%lld entry_to_marker_us=%lld interrupted_task=%s core=%d priority=%u",
                 static_cast<unsigned>(e.sequence),
                 static_cast<unsigned>(e.boundary),
                 static_cast<long long>(e.target_local_us),
                 static_cast<long long>(e.callback_entry_us),
                 static_cast<long long>(e.callback_entry_us - e.target_local_us),
                 static_cast<long long>(e.marker_begin_us),
                 static_cast<long long>(e.marker_end_us),
                 static_cast<long long>(e.marker_begin_us - e.callback_entry_us),
                 task ? task->name : "UNKNOWN",
                 task ? static_cast<int>(task->core_id) : -2,
                 task ? static_cast<unsigned>(task->priority) : 0U);
    }
#else
#endif
}

void cpu0_latency_monitor_get_summary(cpu0_latency_monitor_summary_t* out_summary) {
    if (out_summary == nullptr) return;
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    portENTER_CRITICAL(&s_summary_mux);
    *out_summary = s_summary;
    portEXIT_CRITICAL(&s_summary_mux);
#else
    *out_summary = {};
#endif
}
