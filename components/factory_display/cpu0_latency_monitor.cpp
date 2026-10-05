#include "cpu0_latency_monitor.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtc_discipline.h"
#include "sdkconfig.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

namespace {

constexpr char kTag[] = "cpu0_latency";

#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
constexpr uint32_t kPeriodUs = CONFIG_FACTORY_CPU0_LATENCY_PERIOD_US;
constexpr uint32_t kThresholdUs = CONFIG_FACTORY_CPU0_LATENCY_THRESHOLD_US;
constexpr int64_t kBoundaryGuardUs = CONFIG_FACTORY_CPU0_LATENCY_BOUNDARY_GUARD_US;
constexpr uint32_t kProbeGpio = CONFIG_FACTORY_CPU0_LATENCY_PROBE_GPIO;
constexpr uint32_t kProbePulseUs = 4;
constexpr size_t kEventCapacity = 256;
constexpr size_t kCommitEventCapacity = 32;
constexpr size_t kTaskInventoryCapacity = 48;
constexpr int64_t kCanaryOffsetUs = 15500000LL;
constexpr uint32_t kCanaryHoldUs = CONFIG_FACTORY_CPU0_LATENCY_CANARY_HOLD_US;
constexpr int64_t kIsrCanaryOffsetUs = 16500000LL;
constexpr uint32_t kIsrCanaryHoldUs = CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY_HOLD_US;
constexpr int kCommitInterruptLevel = CONFIG_ESP_TIMER_INTERRUPT_LEVEL;
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
static_assert(kProbeGpio <= 33, "Classic ESP32 latency probe supports GPIO0..33");
#if defined(CONFIG_FACTORY_START_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_START_EDGE_DIAGNOSTICS
static_assert(kProbeGpio != CONFIG_FACTORY_START_EDGE_GPIO,
              "Latency probe GPIO overlaps START-edge GPIO");
#endif
#if defined(CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS
static_assert(kProbeGpio != CONFIG_FACTORY_DISPLAY_EDGE_GPIO,
              "Latency probe GPIO overlaps COMMIT GPIO");
#endif
#if defined(CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS
static_assert(kProbeGpio != CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_GPIO,
              "Latency probe GPIO overlaps REFRESH GPIO");
#endif

struct LatencyEvent {
    uint32_t sequence{};
    uint64_t alarm_count{};
    uint64_t actual_count{};
    uint32_t lateness_us{};
    int64_t expected_local_us{};
    int64_t actual_local_us{};
    TaskHandle_t interrupted_task{};
    int64_t commit_target_local_us{};
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

struct CanaryRecord {
    bool requested{};
    bool completed{};
    int64_t target_disciplined_us{};
    int64_t requested_local_us{};
    int64_t critical_begin_us{};
    int64_t critical_end_us{};
    uint32_t hold_us{};
};

struct IsrCanaryRecord {
    bool requested{};
    bool completed{};
    int64_t requested_after_begin_us{};
    int64_t isr_begin_us{};
    int64_t isr_end_us{};
    uint32_t hold_us{};
    TaskHandle_t interrupted_task{};
};

DRAM_ATTR gptimer_handle_t s_timer = nullptr;
DRAM_ATTR esp_timer_handle_t s_isr_canary_timer = nullptr;
DRAM_ATTR volatile bool s_initialized = false;
DRAM_ATTR volatile bool s_run_active = false;
DRAM_ATTR volatile uint64_t s_run_command_id = 0;
DRAM_ATTR volatile int64_t s_commit_target_local_us = 0;
DRAM_ATTR volatile uint32_t s_event_sequence = 0;
DRAM_ATTR volatile uint32_t s_event_count = 0;
DRAM_ATTR volatile uint32_t s_event_overflow = 0;
DRAM_ATTR volatile uint32_t s_sample_callbacks = 0;
DRAM_ATTR volatile uint32_t s_wrong_core_callbacks = 0;
DRAM_ATTR volatile uint32_t s_guarded_samples = 0;
DRAM_ATTR volatile uint32_t s_probe_suppressed_events = 0;
DRAM_ATTR volatile int64_t s_expected_sample_local_us = 0;
DRAM_ATTR LatencyEvent s_events[kEventCapacity]{};
DRAM_ATTR volatile uint32_t s_commit_event_sequence = 0;
DRAM_ATTR volatile uint32_t s_commit_event_count = 0;
DRAM_ATTR volatile uint32_t s_commit_event_overflow = 0;
DRAM_ATTR CommitLateEvent s_commit_events[kCommitEventCapacity]{};
DRAM_ATTR portMUX_TYPE s_canary_mux = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR CanaryRecord s_canary{};
DRAM_ATTR IsrCanaryRecord s_isr_canary{};
TaskHandle_t s_canary_task = nullptr;
int64_t s_run_start_disciplined_us = 0;
TaskIdentity s_task_inventory[kTaskInventoryCapacity]{};
size_t s_task_inventory_count = 0;

inline void IRAM_ATTR ProbeHigh() {
    if constexpr (kProbeGpio < 32U) {
        REG_WRITE(GPIO_OUT_W1TS_REG, (1U << kProbeGpio));
    } else {
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1U << (kProbeGpio - 32U)));
    }
}

inline void IRAM_ATTR ProbeLow() {
    if constexpr (kProbeGpio < 32U) {
        REG_WRITE(GPIO_OUT_W1TC_REG, (1U << kProbeGpio));
    } else {
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1U << (kProbeGpio - 32U)));
    }
}

inline void IRAM_ATTR ProbePulse() {
    ProbeHigh();
    esp_rom_delay_us(kProbePulseUs);
    ProbeLow();
}

bool IRAM_ATTR ExpectedSampleIsGuarded(int64_t expected_local_us) {
    const int64_t target = s_commit_target_local_us;
    if (target <= 0) return false;
    int64_t delta = expected_local_us - target;
    if (delta < 0) delta = -delta;
    return delta <= kBoundaryGuardUs;
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

bool IRAM_ATTR OnAlarm(gptimer_handle_t,
                       const gptimer_alarm_event_data_t* edata,
                       void*) {
    const int64_t actual_local_us = esp_timer_get_time();
    int64_t expected_local_us = s_expected_sample_local_us;

    // Hardware auto-reload keeps the GPTimer periodic without calling the
    // driver control path at 4 kHz. Anchor the software schedule to the first
    // observed callback, then maintain its absolute 250 us cadence. If CPU0
    // was masked across multiple periods, one pending callback reports the
    // full delay and the schedule advances to the first future sample.
    if (expected_local_us <= 0) {
        s_expected_sample_local_us = actual_local_us + static_cast<int64_t>(kPeriodUs);
        return false;
    }

    int64_t late64 = actual_local_us - expected_local_us;
    if (late64 < 0) late64 = 0;
    const uint32_t lateness_us = late64 > static_cast<int64_t>(UINT32_MAX)
        ? UINT32_MAX
        : static_cast<uint32_t>(late64);

    uint64_t periods_elapsed = 1;
    if (late64 >= static_cast<int64_t>(kPeriodUs)) {
        periods_elapsed += static_cast<uint64_t>(late64 / static_cast<int64_t>(kPeriodUs));
    }
    s_expected_sample_local_us =
        expected_local_us + static_cast<int64_t>(periods_elapsed * static_cast<uint64_t>(kPeriodUs));

    if (!s_run_active) return false;
    s_sample_callbacks = s_sample_callbacks + 1U;
    if (xPortGetCoreID() != 0) {
        s_wrong_core_callbacks = s_wrong_core_callbacks + 1U;
    }

    // Boundary guard: the 4 kHz hardware alarm remains periodic and still
    // measures/retains latency near COMMIT. Only the Analyzer GPIO32 pulse is
    // suppressed there. This preserves guaranteed CPU0-mask coverage without
    // adding a near-coincident diagnostic edge beside GPIO33.
    const bool near_commit = ExpectedSampleIsGuarded(expected_local_us);
    if (near_commit) {
        // Keep the sample active so a 300+ us CPU0 mask that delays COMMIT
        // cannot disappear inside the boundary guard. Only suppress the
        // Analyzer GPIO pulse near GPIO33; retention and attribution remain on.
        s_guarded_samples = s_guarded_samples + 1U;
    }

    if (lateness_us >= kThresholdUs) {
        const TaskHandle_t interrupted = xTaskGetCurrentTaskHandleForCore(0);
        const uint32_t sequence = s_event_sequence + 1U;
        s_event_sequence = sequence;
        const uint32_t count = s_event_count;
        if (count < kEventCapacity) {
            LatencyEvent& event = s_events[count];
            event.sequence = sequence;
            event.alarm_count = edata->alarm_value;
            event.actual_count = edata->count_value;
            event.lateness_us = lateness_us;
            event.expected_local_us = expected_local_us;
            event.actual_local_us = actual_local_us;
            event.interrupted_task = interrupted;
            event.commit_target_local_us = s_commit_target_local_us;
            s_event_count = count + 1U;
            if (!near_commit) {
                ProbePulse();
            } else {
                s_probe_suppressed_events = s_probe_suppressed_events + 1U;
            }
        } else {
            s_event_overflow = s_event_overflow + 1U;
        }
    }

    return false;
}

void IRAM_ATTR IsrCanaryCallback(void*) {
    if (!s_run_active) return;
    s_isr_canary.isr_begin_us = esp_timer_get_time();
    s_isr_canary.interrupted_task = xTaskGetCurrentTaskHandleForCore(0);
    esp_rom_delay_us(kIsrCanaryHoldUs);
    s_isr_canary.isr_end_us = esp_timer_get_time();
    s_isr_canary.completed = true;
}

void CanaryTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_run_active || s_run_start_disciplined_us <= 0) continue;

        const int64_t target_disciplined_us = s_run_start_disciplined_us + kCanaryOffsetUs;
        s_canary.requested = true;
        s_canary.completed = false;
        s_canary.target_disciplined_us = target_disciplined_us;
        s_canary.hold_us = kCanaryHoldUs;

        while (s_run_active) {
            const int64_t local_target_us =
                rtc_discipline_disciplined_to_local_us(target_disciplined_us);
            const int64_t now_us = esp_timer_get_time();
            const int64_t remaining_us = local_target_us - now_us;
            s_canary.requested_local_us = local_target_us;
            if (remaining_us <= 0) break;
            if (remaining_us > 3000) {
                const uint32_t coarse_ms = static_cast<uint32_t>((remaining_us - 2000) / 1000);
                const TickType_t ticks = pdMS_TO_TICKS(coarse_ms);
                if (ticks > 0) {
                    vTaskDelay(ticks);
                    continue;
                }
            }
            while (s_run_active && esp_timer_get_time() < local_target_us) {
                // diagnostic-only short precision wait
            }
            break;
        }

        if (!s_run_active) continue;
        portENTER_CRITICAL(&s_canary_mux);
        s_canary.critical_begin_us = esp_timer_get_time();
        esp_rom_delay_us(kCanaryHoldUs);
        s_canary.critical_end_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_canary_mux);
        s_canary.completed = true;
    }
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

    gpio_config_t probe{};
    probe.pin_bit_mask = 1ULL << kProbeGpio;
    probe.mode = GPIO_MODE_OUTPUT;
    probe.pull_up_en = GPIO_PULLUP_DISABLE;
    probe.pull_down_en = GPIO_PULLDOWN_DISABLE;
    probe.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&probe) != ESP_OK) return false;
    gpio_set_level(static_cast<gpio_num_t>(kProbeGpio), 0);

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

    gptimer_alarm_config_t alarm{};
    alarm.alarm_count = kPeriodUs;
    alarm.reload_count = 0;
    alarm.flags.auto_reload_on_alarm = true;
    if (gptimer_set_alarm_action(s_timer, &alarm) != ESP_OK) return false;
    s_expected_sample_local_us = 0;
    if (gptimer_start(s_timer) != ESP_OK) return false;

    esp_timer_create_args_t isr_canary_args{};
    isr_canary_args.callback = &IsrCanaryCallback;
    isr_canary_args.dispatch_method = ESP_TIMER_ISR;
    isr_canary_args.name = "lat_isr_canary";
    if (esp_timer_create(&isr_canary_args, &s_isr_canary_timer) != ESP_OK) return false;

    if (xTaskCreatePinnedToCore(&CanaryTask,
                                "lat_canary",
                                2048,
                                nullptr,
                                22,
                                &s_canary_task,
                                0) != pdPASS) {
        return false;
    }

    s_initialized = true;
    ESP_LOGI(kTag,
             "CPU0 latency monitor ready: gptimer=4kHz periodic_hw=1 period_us=%u threshold_us=%u guard_us=%lld probe_gpio=%u task_canary_hold_us=%u task_canary_offset_us=%lld isr_canary_hold_us=%u isr_canary_offset_us=%lld sampler_intr_level=%d commit_intr_level=%d core=0",
             static_cast<unsigned>(kPeriodUs),
             static_cast<unsigned>(kThresholdUs),
             static_cast<long long>(kBoundaryGuardUs),
             static_cast<unsigned>(kProbeGpio),
             static_cast<unsigned>(kCanaryHoldUs),
             static_cast<long long>(kCanaryOffsetUs),
             static_cast<unsigned>(kIsrCanaryHoldUs),
             static_cast<long long>(kIsrCanaryOffsetUs),
             kCommitInterruptLevel,
             kCommitInterruptLevel);
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
    s_run_start_disciplined_us = 0;
    s_commit_target_local_us = 0;
    s_event_sequence = 0;
    s_event_count = 0;
    s_event_overflow = 0;
    s_sample_callbacks = 0;
    s_wrong_core_callbacks = 0;
    s_guarded_samples = 0;
    s_probe_suppressed_events = 0;
    s_commit_event_sequence = 0;
    s_commit_event_count = 0;
    s_commit_event_overflow = 0;
    std::memset(s_events, 0, sizeof(s_events));
    std::memset(s_commit_events, 0, sizeof(s_commit_events));
    s_canary = CanaryRecord{};
    s_isr_canary = IsrCanaryRecord{};
    if (s_isr_canary_timer != nullptr) {
        (void)esp_timer_stop(s_isr_canary_timer);
    }
    RefreshTaskInventory();
    ESP_LOGI(kTag,
             "CPU0_LATENCY_RUN_PREP command=%016llX task_inventory=%u",
             static_cast<unsigned long long>(command_id),
             static_cast<unsigned>(s_task_inventory_count));
#else
    (void)command_id;
#endif
}

void cpu0_latency_monitor_begin_run(uint64_t command_id, int64_t start_disciplined_us) {
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    if (!s_initialized) return;
    s_run_command_id = command_id;
    s_run_start_disciplined_us = start_disciplined_us;
    s_run_active = true;
    if (s_canary_task != nullptr) xTaskNotifyGive(s_canary_task);
    if (s_isr_canary_timer != nullptr) {
        (void)esp_timer_stop(s_isr_canary_timer);
        s_isr_canary.requested = true;
        s_isr_canary.completed = false;
        s_isr_canary.requested_after_begin_us = kIsrCanaryOffsetUs;
        s_isr_canary.hold_us = kIsrCanaryHoldUs;
        if (esp_timer_start_once(s_isr_canary_timer, kIsrCanaryOffsetUs) != ESP_OK) {
            s_isr_canary.requested = false;
        }
    }
    ESP_LOGI(kTag,
             "CPU0_LATENCY_RUN_BEGIN command=%016llX start_disciplined_us=%lld task_inventory=%u heartbeat_traffic=%s",
             static_cast<unsigned long long>(command_id),
             static_cast<long long>(start_disciplined_us),
             static_cast<unsigned>(s_task_inventory_count),
             kHeartbeatTraffic);
#else
    (void)command_id;
    (void)start_disciplined_us;
#endif
}

void cpu0_latency_monitor_end_run() {
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    s_run_active = false;
    s_commit_target_local_us = 0;
    if (s_isr_canary_timer != nullptr) {
        (void)esp_timer_stop(s_isr_canary_timer);
    }
#endif
}

void cpu0_latency_monitor_set_commit_target(int64_t target_local_us) {
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    s_commit_target_local_us = target_local_us;
#else
    (void)target_local_us;
#endif
}

void IRAM_ATTR cpu0_latency_monitor_note_commit(uint32_t boundary,
                                                int64_t target_local_us,
                                                int64_t callback_entry_us,
                                                int64_t marker_begin_us,
                                                int64_t marker_end_us) {
#if defined(CONFIG_FACTORY_CPU0_LATENCY_MONITOR) && CONFIG_FACTORY_CPU0_LATENCY_MONITOR
    const int64_t lateness = callback_entry_us - target_local_us;
    if (!s_run_active || lateness < static_cast<int64_t>(kThresholdUs)) return;

    const TaskHandle_t interrupted = xTaskGetCurrentTaskHandleForCore(0);
    const uint32_t sequence = s_commit_event_sequence + 1U;
    s_commit_event_sequence = sequence;
    const uint32_t count = s_commit_event_count;
    if (count < kCommitEventCapacity) {
        CommitLateEvent& event = s_commit_events[count];
        event.sequence = sequence;
        event.boundary = boundary;
        event.target_local_us = target_local_us;
        event.callback_entry_us = callback_entry_us;
        event.marker_begin_us = marker_begin_us;
        event.marker_end_us = marker_end_us;
        event.interrupted_task = interrupted;
        s_commit_event_count = count + 1U;
    } else {
        s_commit_event_overflow = s_commit_event_overflow + 1U;
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
    ESP_LOGI(kTag,
             "CPU0_LATENCY_DIAG command=%016llX period_us=%u threshold_us=%u guard_us=%lld sample_callbacks=%u wrong_core_callbacks=%u guarded_samples=%u events=%u overflow=%u commit_late_events=%u commit_overflow=%u probe_gpio=%u probe_suppressed_events=%u sampler_intr_level=%d commit_intr_level=%d",
             static_cast<unsigned long long>(s_run_command_id),
             static_cast<unsigned>(kPeriodUs),
             static_cast<unsigned>(kThresholdUs),
             static_cast<long long>(kBoundaryGuardUs),
             static_cast<unsigned>(s_sample_callbacks),
             static_cast<unsigned>(s_wrong_core_callbacks),
             static_cast<unsigned>(s_guarded_samples),
             static_cast<unsigned>(s_event_count),
             static_cast<unsigned>(s_event_overflow),
             static_cast<unsigned>(s_commit_event_count),
             static_cast<unsigned>(s_commit_event_overflow),
             static_cast<unsigned>(kProbeGpio),
             static_cast<unsigned>(s_probe_suppressed_events),
             kCommitInterruptLevel,
             kCommitInterruptLevel);

    for (uint32_t i = 0; i < s_event_count && i < kEventCapacity; ++i) {
        const LatencyEvent& e = s_events[i];
        const TaskIdentity* task = FindTaskIdentity(e.interrupted_task);
        ESP_LOGW(kTag,
                 "CPU0_LATENCY_EVENT seq=%u lateness_us=%u expected_local_us=%lld actual_local_us=%lld alarm_count=%llu actual_count=%llu interrupted_task=%s core=%d priority=%u handle=%p commit_target_local_us=%lld",
                 static_cast<unsigned>(e.sequence),
                 static_cast<unsigned>(e.lateness_us),
                 static_cast<long long>(e.expected_local_us),
                 static_cast<long long>(e.actual_local_us),
                 static_cast<unsigned long long>(e.alarm_count),
                 static_cast<unsigned long long>(e.actual_count),
                 task ? task->name : "UNKNOWN",
                 task ? static_cast<int>(task->core_id) : -2,
                 task ? static_cast<unsigned>(task->priority) : 0U,
                 e.interrupted_task,
                 static_cast<long long>(e.commit_target_local_us));
    }

    for (uint32_t i = 0; i < s_commit_event_count && i < kCommitEventCapacity; ++i) {
        const CommitLateEvent& e = s_commit_events[i];
        const TaskIdentity* task = FindTaskIdentity(e.interrupted_task);
        ESP_LOGW(kTag,
                 "CPU0_COMMIT_LATE seq=%u boundary=%u target_local_us=%lld callback_entry_us=%lld callback_lateness_us=%lld marker_begin_us=%lld marker_end_us=%lld entry_to_marker_us=%lld interrupted_task=%s core=%d priority=%u handle=%p",
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
                 task ? static_cast<unsigned>(task->priority) : 0U,
                 e.interrupted_task);
    }

    const TaskIdentity* canary_task = FindTaskIdentity(s_canary_task);
    ESP_LOGI(kTag,
             "CPU0_LATENCY_CANARY requested=%u completed=%u target_disciplined_us=%lld requested_local_us=%lld critical_begin_us=%lld critical_end_us=%lld duration_us=%lld hold_us=%u task=%s handle=%p",
             s_canary.requested ? 1U : 0U,
             s_canary.completed ? 1U : 0U,
             static_cast<long long>(s_canary.target_disciplined_us),
             static_cast<long long>(s_canary.requested_local_us),
             static_cast<long long>(s_canary.critical_begin_us),
             static_cast<long long>(s_canary.critical_end_us),
             static_cast<long long>(s_canary.critical_end_us - s_canary.critical_begin_us),
             static_cast<unsigned>(s_canary.hold_us),
             canary_task ? canary_task->name : "lat_canary",
             s_canary_task);

    const TaskIdentity* isr_under_task = FindTaskIdentity(s_isr_canary.interrupted_task);
    ESP_LOGI(kTag,
             "CPU0_ISR_CANARY requested=%u completed=%u requested_after_begin_us=%lld isr_begin_us=%lld isr_end_us=%lld duration_us=%lld hold_us=%u interrupted_task=%s core=%d priority=%u handle=%p",
             s_isr_canary.requested ? 1U : 0U,
             s_isr_canary.completed ? 1U : 0U,
             static_cast<long long>(s_isr_canary.requested_after_begin_us),
             static_cast<long long>(s_isr_canary.isr_begin_us),
             static_cast<long long>(s_isr_canary.isr_end_us),
             static_cast<long long>(s_isr_canary.isr_end_us - s_isr_canary.isr_begin_us),
             static_cast<unsigned>(s_isr_canary.hold_us),
             isr_under_task ? isr_under_task->name : "UNKNOWN",
             isr_under_task ? static_cast<int>(isr_under_task->core_id) : -2,
             isr_under_task ? static_cast<unsigned>(isr_under_task->priority) : 0U,
             s_isr_canary.interrupted_task);
#else
#endif
}
