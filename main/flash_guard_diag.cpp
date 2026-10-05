#include "flash_guard_diag.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_flash_chips/esp_flash_types.h"
#include "esp_log.h"
#include "esp_private/cache_utils.h"
#include "esp_rom_sys.h"
#include "esp_spi_flash_counters.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "nvs.h"
#include "rtc_discipline.h"
#include "sdkconfig.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"

namespace {

constexpr char kTag[] = "flash_guard_diag";
constexpr uint32_t kRingCapacity = 256;
#if defined(CONFIG_FACTORY_FLASH_GUARD_EVENT_THRESHOLD_US)
constexpr uint32_t kEventThresholdUs = CONFIG_FACTORY_FLASH_GUARD_EVENT_THRESHOLD_US;
#else
constexpr uint32_t kEventThresholdUs = 0;
#endif

#if defined(CONFIG_FACTORY_TIMING_PROBE_OUTPUT) && CONFIG_FACTORY_TIMING_PROBE_OUTPUT
constexpr uint32_t kProbeGpio = CONFIG_FACTORY_TIMING_PROBE_GPIO;
static_assert(kProbeGpio <= 33, "v6.23.8 timing probe supports classic ESP32 GPIO0..33");
#if defined(CONFIG_FACTORY_START_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_START_EDGE_DIAGNOSTICS
static_assert(kProbeGpio != CONFIG_FACTORY_START_EDGE_GPIO,
              "Timing probe GPIO must not overlap the physical START-edge diagnostic GPIO");
#endif
#endif

struct FlashEvent {
    uint32_t sequence{};
    int64_t start_hook_enter_us{};
    int64_t cache_off_begin_us{};
    int64_t cache_off_end_us{};
    int64_t end_hook_return_us{};
    uint32_t core{};
};

struct NvsSnapshot {
    bool valid{};
    size_t used_entries{};
    size_t free_entries{};
    size_t total_entries{};
    size_t namespace_count{};
};

struct FlashCounterSnapshot {
    bool valid{};
    uint64_t read_count{};
    uint64_t read_time_us{};
    uint64_t read_bytes{};
    uint64_t write_count{};
    uint64_t write_time_us{};
    uint64_t write_bytes{};
    uint64_t erase_count{};
    uint64_t erase_time_us{};
    uint64_t erase_bytes{};
};

enum class MechanismKind : uint32_t {
    CacheOff = 1,
    Cpu0Critical = 2,
    Cpu1Critical = 3,
};

struct MechanismPlan {
    uint32_t sequence{};
    MechanismKind kind{};
    uint32_t boundary{};
    BaseType_t core{};
    int64_t start_lead_us{};
    uint32_t hold_us{};
    TaskHandle_t task{};
    esp_timer_handle_t wake_timer{};
    int64_t target_local_us{};
    int64_t requested_start_local_us{};
    int64_t wake_local_us{};
    esp_err_t timer_start_result{ESP_ERR_INVALID_STATE};
};

struct MechanismEvent {
    uint32_t sequence{};
    MechanismKind kind{};
    uint32_t boundary{};
    uint32_t core{};
    int64_t target_local_us{};
    int64_t requested_start_local_us{};
    int64_t worker_awake_us{};
    int64_t operation_call_begin_us{};
    int64_t protected_begin_us{};
    int64_t protected_end_us{};
    int64_t operation_call_end_us{};
    uint32_t requested_hold_us{};
    bool completed{};
};

// Main esp_flash path instrumentation. The hook-entry timestamp measures lock/IPC
// acquisition latency separately from the cache-disabled interval itself.
DRAM_ATTR const esp_flash_os_functions_t *s_original_flash_os = nullptr;
DRAM_ATTR esp_flash_os_functions_t s_diagnostic_flash_os{};
DRAM_ATTR volatile int64_t s_active_start_hook_enter_us = 0;
DRAM_ATTR volatile int64_t s_active_cache_off_begin_us = 0;
DRAM_ATTR volatile uint32_t s_active_core = 0;
DRAM_ATTR volatile uint32_t s_total_operations = 0;
DRAM_ATTR volatile uint32_t s_total_duration_us = 0;
DRAM_ATTR volatile uint32_t s_max_duration_us = 0;
DRAM_ATTR volatile uint32_t s_event_write_sequence = 0;
DRAM_ATTR FlashEvent s_events[kRingCapacity]{};

uint64_t s_run_command_id = 0;
int64_t s_run_target_local_us = 0;
uint32_t s_run_total_operations = 0;
uint32_t s_run_total_duration_us = 0;
uint32_t s_run_event_sequence = 0;
NvsSnapshot s_run_nvs{};
FlashCounterSnapshot s_run_flash_counters{};

#if defined(CONFIG_FACTORY_TIMING_MECHANISM_TEST) && CONFIG_FACTORY_TIMING_MECHANISM_TEST
constexpr int64_t kMechanismWakeAdvanceUs = 800;
constexpr uint32_t kMechanismPlanCount = 3;
MechanismPlan s_mechanism_plans[kMechanismPlanCount] = {
    {1, MechanismKind::CacheOff, 30, 0, 500, 1000},
    {2, MechanismKind::Cpu0Critical, 60, 0, 100, 300},
    {3, MechanismKind::Cpu1Critical, 90, 1, 100, 300},
};
DRAM_ATTR MechanismEvent s_mechanism_events[kMechanismPlanCount]{};
std::atomic<bool> s_mechanism_active{false};
int64_t s_mechanism_disciplined_start_us = 0;
uint64_t s_mechanism_command_id = 0;
DRAM_ATTR portMUX_TYPE s_cpu0_test_mux = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR portMUX_TYPE s_cpu1_test_mux = portMUX_INITIALIZER_UNLOCKED;
#endif

NvsSnapshot ReadNvsStats() {
    nvs_stats_t stats{};
    NvsSnapshot snapshot{};
    if (nvs_get_stats(nullptr, &stats) == ESP_OK) {
        snapshot.valid = true;
        snapshot.used_entries = stats.used_entries;
        snapshot.free_entries = stats.free_entries;
        snapshot.total_entries = stats.total_entries;
        snapshot.namespace_count = stats.namespace_count;
    }
    return snapshot;
}

FlashCounterSnapshot ReadFlashCounters() {
    FlashCounterSnapshot snapshot{};
#if defined(CONFIG_SPI_FLASH_ENABLE_COUNTERS) && CONFIG_SPI_FLASH_ENABLE_COUNTERS
    const esp_flash_counters_t *counters = esp_flash_get_counters();
    if (counters != nullptr) {
        snapshot.valid = true;
        snapshot.read_count = counters->read.count;
        snapshot.read_time_us = counters->read.time;
        snapshot.read_bytes = counters->read.bytes;
        snapshot.write_count = counters->write.count;
        snapshot.write_time_us = counters->write.time;
        snapshot.write_bytes = counters->write.bytes;
        snapshot.erase_count = counters->erase.count;
        snapshot.erase_time_us = counters->erase.time;
        snapshot.erase_bytes = counters->erase.bytes;
    }
#endif
    return snapshot;
}

inline void IRAM_ATTR ProbeHigh() {
#if defined(CONFIG_FACTORY_TIMING_PROBE_OUTPUT) && CONFIG_FACTORY_TIMING_PROBE_OUTPUT
    if constexpr (kProbeGpio < 32U) {
        REG_WRITE(GPIO_OUT_W1TS_REG, (1U << kProbeGpio));
    } else {
        REG_WRITE(GPIO_OUT1_W1TS_REG, (1U << (kProbeGpio - 32U)));
    }
#endif
}

inline void IRAM_ATTR ProbeLow() {
#if defined(CONFIG_FACTORY_TIMING_PROBE_OUTPUT) && CONFIG_FACTORY_TIMING_PROBE_OUTPUT
    if constexpr (kProbeGpio < 32U) {
        REG_WRITE(GPIO_OUT_W1TC_REG, (1U << kProbeGpio));
    } else {
        REG_WRITE(GPIO_OUT1_W1TC_REG, (1U << (kProbeGpio - 32U)));
    }
#endif
}

void IRAM_ATTR RecordFlashInterval(int64_t start_hook_enter_us,
                                   int64_t cache_off_begin_us,
                                   int64_t cache_off_end_us,
                                   int64_t end_hook_return_us,
                                   uint32_t core) {
    const int64_t duration64 = cache_off_end_us >= cache_off_begin_us
        ? cache_off_end_us - cache_off_begin_us
        : 0;
    const uint32_t duration_us = duration64 > static_cast<int64_t>(UINT32_MAX)
        ? UINT32_MAX
        : static_cast<uint32_t>(duration64);

    const uint32_t total_operations = s_total_operations;
    s_total_operations = total_operations + 1U;
    const uint32_t total_duration_us = s_total_duration_us;
    s_total_duration_us = total_duration_us + duration_us;
    if (duration_us > s_max_duration_us) {
        s_max_duration_us = duration_us;
    }

    if (duration_us < kEventThresholdUs) {
        return;
    }

    const uint32_t sequence = s_event_write_sequence + 1U;
    const uint32_t index = (sequence - 1U) % kRingCapacity;
    s_events[index].sequence = 0;
    s_events[index].start_hook_enter_us = start_hook_enter_us;
    s_events[index].cache_off_begin_us = cache_off_begin_us;
    s_events[index].cache_off_end_us = cache_off_end_us;
    s_events[index].end_hook_return_us = end_hook_return_us;
    s_events[index].core = core;
    s_events[index].sequence = sequence;
    s_event_write_sequence = sequence;
}

esp_err_t IRAM_ATTR DiagnosticFlashOsStart(void *arg, uint32_t flags) {
    const esp_flash_os_functions_t *original = s_original_flash_os;
    if (original == nullptr || original->start == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t hook_enter_us = esp_timer_get_time();
    const uint32_t core = static_cast<uint32_t>(xPortGetCoreID());
    const esp_err_t result = original->start(arg, flags);
    if (result == ESP_OK) {
        // original->start() has acquired the flash/IPC path and disabled cache.
        ProbeHigh();
        s_active_start_hook_enter_us = hook_enter_us;
        s_active_cache_off_begin_us = esp_timer_get_time();
        s_active_core = core;
    }
    return result;
}

esp_err_t IRAM_ATTR DiagnosticFlashOsEnd(void *arg) {
    const int64_t cache_off_end_us = esp_timer_get_time();
    ProbeLow();

    const esp_flash_os_functions_t *original = s_original_flash_os;
    if (original == nullptr || original->end == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = original->end(arg);
    const int64_t hook_return_us = esp_timer_get_time();

    const int64_t hook_enter_us = s_active_start_hook_enter_us;
    const int64_t cache_begin_us = s_active_cache_off_begin_us;
    const uint32_t core = s_active_core;
    if (hook_enter_us > 0 && cache_begin_us > 0 && cache_off_end_us >= cache_begin_us) {
        RecordFlashInterval(hook_enter_us,
                            cache_begin_us,
                            cache_off_end_us,
                            hook_return_us,
                            core);
    }
    s_active_start_hook_enter_us = 0;
    s_active_cache_off_begin_us = 0;
    s_active_core = 0;
    return result;
}

#if defined(CONFIG_FACTORY_TIMING_MECHANISM_TEST) && CONFIG_FACTORY_TIMING_MECHANISM_TEST
const char *MechanismKindName(MechanismKind kind) {
    switch (kind) {
        case MechanismKind::CacheOff: return "CACHE_OFF";
        case MechanismKind::Cpu0Critical: return "CPU0_CRITICAL";
        case MechanismKind::Cpu1Critical: return "CPU1_CRITICAL";
    }
    return "UNKNOWN";
}

void IRAM_ATTR InjectCacheOffWindow(uint32_t hold_us,
                                    MechanismEvent *event) {
    event->operation_call_begin_us = esp_timer_get_time();
    spi_flash_disable_interrupts_caches_and_other_cpu();
    event->protected_begin_us = esp_timer_get_time();
    ProbeHigh();
    esp_rom_delay_us(hold_us);
    ProbeLow();
    event->protected_end_us = esp_timer_get_time();
    spi_flash_enable_interrupts_caches_and_other_cpu();
    event->operation_call_end_us = esp_timer_get_time();
}

void IRAM_ATTR InjectCriticalWindow(portMUX_TYPE *mux,
                                    uint32_t hold_us,
                                    MechanismEvent *event) {
    event->operation_call_begin_us = esp_timer_get_time();
    portENTER_CRITICAL(mux);
    event->protected_begin_us = esp_timer_get_time();
    ProbeHigh();
    esp_rom_delay_us(hold_us);
    ProbeLow();
    event->protected_end_us = esp_timer_get_time();
    portEXIT_CRITICAL(mux);
    event->operation_call_end_us = esp_timer_get_time();
}

void MechanismWakeTimerCallback(void *arg) {
    auto *plan = static_cast<MechanismPlan *>(arg);
    if (plan == nullptr || plan->task == nullptr ||
        !s_mechanism_active.load(std::memory_order_relaxed)) {
        return;
    }
    xTaskNotifyGive(plan->task);
}

void MechanismWorkerTask(void *arg) {
    auto *plan = static_cast<MechanismPlan *>(arg);
    if (plan == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_mechanism_active.load(std::memory_order_acquire)) {
            continue;
        }

        // Use the global/internal-DRAM event slot directly. The cache-off test
        // must not depend on a task stack that could ever be placed in external RAM.
        MechanismEvent *event = &s_mechanism_events[plan->sequence - 1U];
        *event = MechanismEvent{};
        event->sequence = plan->sequence;
        event->kind = plan->kind;
        event->boundary = plan->boundary;
        event->core = static_cast<uint32_t>(xPortGetCoreID());
        event->target_local_us = plan->target_local_us;
        event->requested_start_local_us = plan->requested_start_local_us;
        event->worker_awake_us = esp_timer_get_time();
        event->requested_hold_us = plan->hold_us;

        while (s_mechanism_active.load(std::memory_order_relaxed) &&
               esp_timer_get_time() < event->requested_start_local_us) {
            // Deliberately short (<~0.8 ms) fine wait in a diagnostic-only task.
        }

        if (!s_mechanism_active.load(std::memory_order_relaxed)) {
            continue;
        }

        if (plan->kind == MechanismKind::CacheOff) {
            InjectCacheOffWindow(plan->hold_us, event);
        } else if (plan->kind == MechanismKind::Cpu0Critical) {
            InjectCriticalWindow(&s_cpu0_test_mux, plan->hold_us, event);
        } else {
            InjectCriticalWindow(&s_cpu1_test_mux, plan->hold_us, event);
        }
        event->completed = true;
    }
}

bool InitialiseMechanismPositiveControl() {
#if !(defined(CONFIG_IDF_TARGET_ESP32) && CONFIG_IDF_TARGET_ESP32)
    ESP_LOGW(kTag, "Timing mechanism positive control is supported only on classic ESP32");
    return false;
#else
    for (uint32_t i = 0; i < kMechanismPlanCount; ++i) {
        auto &plan = s_mechanism_plans[i];
        const char *task_name = plan.sequence == 1 ? "cacheoff_test" :
                                plan.sequence == 2 ? "cpu0mask_test" : "cpu1mask_test";
        if (xTaskCreatePinnedToCore(&MechanismWorkerTask,
                                    task_name,
                                    4096,
                                    &plan,
                                    23,
                                    &plan.task,
                                    plan.core) != pdPASS) {
            ESP_LOGW(kTag, "Mechanism task create failed: sequence=%u", static_cast<unsigned>(plan.sequence));
            return false;
        }

        esp_timer_create_args_t args{};
        args.callback = &MechanismWakeTimerCallback;
        args.arg = &plan;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = task_name;
        const esp_err_t timer_result = esp_timer_create(&args, &plan.wake_timer);
        if (timer_result != ESP_OK) {
            ESP_LOGW(kTag,
                     "Mechanism timer create failed: sequence=%u err=%s",
                     static_cast<unsigned>(plan.sequence),
                     esp_err_to_name(timer_result));
            return false;
        }
    }

    ESP_LOGW(kTag,
             "TIMING_MECHANISM_POSITIVE_CONTROL enabled: b30=CACHE_OFF(1000us centered) b60=CPU0_CRITICAL(300us start -100us) b90=CPU1_CRITICAL(300us start -100us) wake_advance_us=%lld; diagnostic builds only",
             static_cast<long long>(kMechanismWakeAdvanceUs));
    return true;
#endif
}

void ScheduleMechanismControls(int64_t actual_local_start_us) {
    s_mechanism_disciplined_start_us =
        rtc_discipline_local_to_disciplined_us(actual_local_start_us);
    s_mechanism_active.store(true, std::memory_order_release);

    for (auto &event : s_mechanism_events) {
        event = MechanismEvent{};
    }

    const int64_t now_us = esp_timer_get_time();
    for (auto &plan : s_mechanism_plans) {
        const int64_t target_disciplined_us = s_mechanism_disciplined_start_us +
            static_cast<int64_t>(plan.boundary) * 1000000LL;
        plan.target_local_us = rtc_discipline_disciplined_to_local_us(target_disciplined_us);
        plan.requested_start_local_us = plan.target_local_us - plan.start_lead_us;
        plan.wake_local_us = plan.requested_start_local_us - kMechanismWakeAdvanceUs;
        int64_t delay_us = plan.wake_local_us - now_us;
        if (delay_us < 1) {
            delay_us = 1;
        }
        plan.timer_start_result = esp_timer_start_once(
            plan.wake_timer, static_cast<uint64_t>(delay_us));
    }
}
#endif

}  // namespace

bool flash_guard_diag_init() {
#if defined(CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS) && CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS
#if defined(CONFIG_FACTORY_TIMING_PROBE_OUTPUT) && CONFIG_FACTORY_TIMING_PROBE_OUTPUT
    gpio_config_t probe_cfg{};
    probe_cfg.pin_bit_mask = (1ULL << kProbeGpio);
    probe_cfg.mode = GPIO_MODE_OUTPUT;
    probe_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    probe_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    probe_cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&probe_cfg) != ESP_OK) {
        ESP_LOGW(kTag, "Timing probe GPIO%u configuration failed", static_cast<unsigned>(kProbeGpio));
        return false;
    }
    gpio_set_level(static_cast<gpio_num_t>(kProbeGpio), 0);
#endif

    if (esp_flash_default_chip == nullptr || esp_flash_default_chip->os_func == nullptr) {
        ESP_LOGW(kTag, "Main esp_flash OS callbacks unavailable; diagnostic disabled");
        return false;
    }
    if (esp_flash_default_chip->os_func != &s_diagnostic_flash_os) {
        const esp_flash_os_functions_t *current = esp_flash_default_chip->os_func;
        if (current->start == nullptr || current->end == nullptr) {
            ESP_LOGW(kTag, "Main esp_flash start/end callbacks unavailable; diagnostic disabled");
            return false;
        }
        s_original_flash_os = current;
        s_diagnostic_flash_os = *current;
        s_diagnostic_flash_os.start = &DiagnosticFlashOsStart;
        s_diagnostic_flash_os.end = &DiagnosticFlashOsEnd;
        esp_flash_default_chip->os_func = &s_diagnostic_flash_os;
    }

    ESP_LOGI(kTag,
             "Main esp_flash OS-window diagnostic installed: threshold_us=%u ring=%u counters=%u probe=%u probe_gpio=%u start_wait_trace=1",
             static_cast<unsigned>(kEventThresholdUs),
             static_cast<unsigned>(kRingCapacity),
#if defined(CONFIG_SPI_FLASH_ENABLE_COUNTERS) && CONFIG_SPI_FLASH_ENABLE_COUNTERS
             1u,
#else
             0u,
#endif
#if defined(CONFIG_FACTORY_TIMING_PROBE_OUTPUT) && CONFIG_FACTORY_TIMING_PROBE_OUTPUT
             1u,
             static_cast<unsigned>(kProbeGpio)
#else
             0u,
             0u
#endif
             );

#if defined(CONFIG_FACTORY_TIMING_MECHANISM_TEST) && CONFIG_FACTORY_TIMING_MECHANISM_TEST
    if (!InitialiseMechanismPositiveControl()) {
        return false;
    }
#endif
    return true;
#else
    return false;
#endif
}

void flash_guard_diag_begin_run(uint64_t command_id, int64_t target_local_start_us) {
#if defined(CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS) && CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS
    s_run_command_id = command_id;
    s_run_target_local_us = target_local_start_us;
    s_run_total_operations = s_total_operations;
    s_run_total_duration_us = s_total_duration_us;
    s_run_event_sequence = s_event_write_sequence;
    s_run_nvs = ReadNvsStats();
    s_run_flash_counters = ReadFlashCounters();
    ProbeLow();

#if defined(CONFIG_FACTORY_TIMING_MECHANISM_TEST) && CONFIG_FACTORY_TIMING_MECHANISM_TEST
    s_mechanism_active.store(false, std::memory_order_relaxed);
    s_mechanism_command_id = command_id;
    for (auto &plan : s_mechanism_plans) {
        if (plan.wake_timer != nullptr) {
            (void)esp_timer_stop(plan.wake_timer);
        }
        plan.target_local_us = 0;
        plan.requested_start_local_us = 0;
        plan.wake_local_us = 0;
        plan.timer_start_result = ESP_ERR_INVALID_STATE;
    }
    for (auto &event : s_mechanism_events) {
        event = MechanismEvent{};
    }
#endif

    ESP_LOGI(kTag,
             "FLASH_OS_RUN_ARM command=%016llX target_local_us=%lld nvs_valid=%u nvs_used=%u nvs_free=%u nvs_total=%u nvs_namespaces=%u flash_counters_valid=%u write_count=%llu write_time_us=%llu write_bytes=%llu",
             static_cast<unsigned long long>(command_id),
             static_cast<long long>(target_local_start_us),
             s_run_nvs.valid ? 1u : 0u,
             static_cast<unsigned>(s_run_nvs.used_entries),
             static_cast<unsigned>(s_run_nvs.free_entries),
             static_cast<unsigned>(s_run_nvs.total_entries),
             static_cast<unsigned>(s_run_nvs.namespace_count),
             s_run_flash_counters.valid ? 1u : 0u,
             static_cast<unsigned long long>(s_run_flash_counters.write_count),
             static_cast<unsigned long long>(s_run_flash_counters.write_time_us),
             static_cast<unsigned long long>(s_run_flash_counters.write_bytes));
#else
    (void)command_id;
    (void)target_local_start_us;
#endif
}

void flash_guard_diag_countdown_started(uint64_t command_id, int64_t actual_local_start_us) {
#if defined(CONFIG_FACTORY_TIMING_MECHANISM_TEST) && CONFIG_FACTORY_TIMING_MECHANISM_TEST
    if (command_id != s_run_command_id || command_id != s_mechanism_command_id) {
        return;
    }
    ScheduleMechanismControls(actual_local_start_us);
#else
    (void)command_id;
    (void)actual_local_start_us;
#endif
}

void flash_guard_diag_countdown_finished() {
#if defined(CONFIG_FACTORY_TIMING_MECHANISM_TEST) && CONFIG_FACTORY_TIMING_MECHANISM_TEST
    s_mechanism_active.store(false, std::memory_order_release);
    for (auto &plan : s_mechanism_plans) {
        if (plan.wake_timer != nullptr) {
            const esp_err_t stop_result = esp_timer_stop(plan.wake_timer);
            (void)stop_result;
        }
    }
#endif
    ProbeLow();
}

void flash_guard_diag_dump_run() {
#if defined(CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS) && CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS
    const uint32_t end_operations = s_total_operations;
    const uint32_t end_duration_us = s_total_duration_us;
    const uint32_t end_event_sequence = s_event_write_sequence;
    const NvsSnapshot end_nvs = ReadNvsStats();
    const FlashCounterSnapshot end_flash_counters = ReadFlashCounters();

    uint32_t first_sequence = s_run_event_sequence + 1U;
    if (end_event_sequence >= kRingCapacity &&
        first_sequence <= end_event_sequence - kRingCapacity) {
        first_sequence = end_event_sequence - kRingCapacity + 1U;
    }

    uint32_t retained_events = 0;
    uint32_t run_max_us = 0;
    for (uint32_t sequence = first_sequence; sequence <= end_event_sequence; ++sequence) {
        const uint32_t index = (sequence - 1U) % kRingCapacity;
        const FlashEvent event = s_events[index];
        if (event.sequence != sequence || event.cache_off_begin_us < s_run_target_local_us) {
            continue;
        }
        const int64_t duration64 = event.cache_off_end_us - event.cache_off_begin_us;
        const uint32_t duration_us = duration64 > 0 ? static_cast<uint32_t>(duration64) : 0;
        ++retained_events;
        run_max_us = std::max(run_max_us, duration_us);
    }

    const uint64_t write_count_delta =
        end_flash_counters.valid && s_run_flash_counters.valid
            ? end_flash_counters.write_count - s_run_flash_counters.write_count
            : 0;
    const uint64_t write_time_delta =
        end_flash_counters.valid && s_run_flash_counters.valid
            ? end_flash_counters.write_time_us - s_run_flash_counters.write_time_us
            : 0;
    const uint64_t write_bytes_delta =
        end_flash_counters.valid && s_run_flash_counters.valid
            ? end_flash_counters.write_bytes - s_run_flash_counters.write_bytes
            : 0;
    const uint64_t erase_count_delta =
        end_flash_counters.valid && s_run_flash_counters.valid
            ? end_flash_counters.erase_count - s_run_flash_counters.erase_count
            : 0;
    const uint64_t erase_time_delta =
        end_flash_counters.valid && s_run_flash_counters.valid
            ? end_flash_counters.erase_time_us - s_run_flash_counters.erase_time_us
            : 0;

    ESP_LOGI(kTag,
             "FLASH_OS_DIAG command=%016llX target_local_us=%lld operations=%u total_us=%u max_retained_us=%u events_ge_%u_us=%u ring_overwritten=%u nvs_start_valid=%u nvs_end_valid=%u nvs_used_start=%u nvs_used_end=%u nvs_free_start=%u nvs_free_end=%u nvs_namespace_start=%u nvs_namespace_end=%u flash_counters_valid=%u write_count_delta=%llu write_time_us_delta=%llu write_bytes_delta=%llu erase_count_delta=%llu erase_time_us_delta=%llu",
             static_cast<unsigned long long>(s_run_command_id),
             static_cast<long long>(s_run_target_local_us),
             static_cast<unsigned>(end_operations - s_run_total_operations),
             static_cast<unsigned>(end_duration_us - s_run_total_duration_us),
             static_cast<unsigned>(run_max_us),
             static_cast<unsigned>(kEventThresholdUs),
             static_cast<unsigned>(retained_events),
             (end_event_sequence - s_run_event_sequence) > kRingCapacity ? 1u : 0u,
             s_run_nvs.valid ? 1u : 0u,
             end_nvs.valid ? 1u : 0u,
             static_cast<unsigned>(s_run_nvs.used_entries),
             static_cast<unsigned>(end_nvs.used_entries),
             static_cast<unsigned>(s_run_nvs.free_entries),
             static_cast<unsigned>(end_nvs.free_entries),
             static_cast<unsigned>(s_run_nvs.namespace_count),
             static_cast<unsigned>(end_nvs.namespace_count),
             end_flash_counters.valid && s_run_flash_counters.valid ? 1u : 0u,
             static_cast<unsigned long long>(write_count_delta),
             static_cast<unsigned long long>(write_time_delta),
             static_cast<unsigned long long>(write_bytes_delta),
             static_cast<unsigned long long>(erase_count_delta),
             static_cast<unsigned long long>(erase_time_delta));

    for (uint32_t sequence = first_sequence; sequence <= end_event_sequence; ++sequence) {
        const uint32_t index = (sequence - 1U) % kRingCapacity;
        const FlashEvent event = s_events[index];
        if (event.sequence != sequence || event.cache_off_begin_us < s_run_target_local_us) {
            continue;
        }
        ESP_LOGI(kTag,
                 "FLASH_OS_EVENT command=%016llX sequence=%u core=%u start_hook_enter_us=%lld cache_off_begin_us=%lld start_wait_us=%lld cache_off_end_us=%lld cache_off_duration_us=%lld end_hook_return_us=%lld end_restore_us=%lld",
                 static_cast<unsigned long long>(s_run_command_id),
                 static_cast<unsigned>(sequence),
                 static_cast<unsigned>(event.core),
                 static_cast<long long>(event.start_hook_enter_us),
                 static_cast<long long>(event.cache_off_begin_us),
                 static_cast<long long>(event.cache_off_begin_us - event.start_hook_enter_us),
                 static_cast<long long>(event.cache_off_end_us),
                 static_cast<long long>(event.cache_off_end_us - event.cache_off_begin_us),
                 static_cast<long long>(event.end_hook_return_us),
                 static_cast<long long>(event.end_hook_return_us - event.cache_off_end_us));
    }

#if defined(CONFIG_FACTORY_TIMING_MECHANISM_TEST) && CONFIG_FACTORY_TIMING_MECHANISM_TEST
    uint32_t completed = 0;
    for (const auto &event : s_mechanism_events) {
        if (event.completed) {
            ++completed;
        }
    }
    ESP_LOGI(kTag,
             "TIMING_MECHANISM_DIAG command=%016llX enabled=1 requested=3 completed=%u probe_gpio=%u wake_advance_us=%lld",
             static_cast<unsigned long long>(s_mechanism_command_id),
             static_cast<unsigned>(completed),
#if defined(CONFIG_FACTORY_TIMING_PROBE_OUTPUT) && CONFIG_FACTORY_TIMING_PROBE_OUTPUT
             static_cast<unsigned>(kProbeGpio),
#else
             0u,
#endif
             static_cast<long long>(kMechanismWakeAdvanceUs));

    for (const auto &event : s_mechanism_events) {
        if (!event.completed) {
            continue;
        }
        ESP_LOGI(kTag,
                 "TIMING_MECHANISM_EVENT command=%016llX sequence=%u kind=%s boundary=%u core=%u target_local_us=%lld requested_start_local_us=%lld worker_awake_us=%lld operation_call_begin_us=%lld call_begin_minus_target_us=%lld protected_begin_us=%lld protected_begin_minus_target_us=%lld protected_end_us=%lld protected_end_minus_target_us=%lld protected_duration_us=%lld operation_call_end_us=%lld requested_hold_us=%u",
                 static_cast<unsigned long long>(s_mechanism_command_id),
                 static_cast<unsigned>(event.sequence),
                 MechanismKindName(event.kind),
                 static_cast<unsigned>(event.boundary),
                 static_cast<unsigned>(event.core),
                 static_cast<long long>(event.target_local_us),
                 static_cast<long long>(event.requested_start_local_us),
                 static_cast<long long>(event.worker_awake_us),
                 static_cast<long long>(event.operation_call_begin_us),
                 static_cast<long long>(event.operation_call_begin_us - event.target_local_us),
                 static_cast<long long>(event.protected_begin_us),
                 static_cast<long long>(event.protected_begin_us - event.target_local_us),
                 static_cast<long long>(event.protected_end_us),
                 static_cast<long long>(event.protected_end_us - event.target_local_us),
                 static_cast<long long>(event.protected_end_us - event.protected_begin_us),
                 static_cast<long long>(event.operation_call_end_us),
                 static_cast<unsigned>(event.requested_hold_us));
    }
#endif
#else
    return;
#endif
}
