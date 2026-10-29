#include "repetition_output.h"
#include "../../main/segment_schedule.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rtc_discipline.h"
#include <algorithm>

#if defined(CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS && CONFIG_FACTORY_DISPLAY_EDGE_GPIO == 33
#error "GPIO33 is the repetition output; move the COMMIT marker to GPIO17"
#endif

namespace factory_output {
namespace {
constexpr gpio_num_t kPin = static_cast<gpio_num_t>(33);
constexpr uint32_t kActiveLevel = 0;
constexpr uint32_t kInactiveLevel = 1;
constexpr int64_t kHalfSecond = 500000;
SemaphoreHandle_t mutex{};
esp_timer_handle_t timer{};
bool ready{}, active{};
int64_t intro_end{}, epoch{};
uint32_t duration{}, repeats{}, logo{}, repeat_start{};

void WriteOutput(bool on) {
    gpio_set_level(kPin, on ? kActiveLevel : kInactiveLevel);
}

// All callers are task context. This short mutex serializes RESET/replacement
// against an already queued timer callback; no allocation, logging or sleeps
// occur in the callback. Deadlines are absolute, never previous-callback+500ms.
void UpdateLocked() {
    (void)esp_timer_stop(timer);
    if (!active) { WriteOutput(false); return; }
    const int64_t local_now = esp_timer_get_time();
    int64_t next_local{};
    bool on{};
    if (local_now < intro_end) {
        on = true;
        next_local = intro_end;
    } else {
        const int64_t disciplined_now = rtc_discipline_local_to_disciplined_us(local_now);
        const int64_t elapsed = disciplined_now - epoch;
        const int64_t cycle = (static_cast<int64_t>(duration) + logo + repeat_start) * 1000000;
        if (elapsed >= static_cast<int64_t>(factory_timer::SegmentSeconds(duration, repeats, logo, repeat_start)) * 1000000) {
            active = false;
            WriteOutput(false);
            return;
        }
        const int64_t index = elapsed < 0 ? 0 : elapsed / cycle;
        const int64_t logo_begin = epoch + index * cycle + static_cast<int64_t>(duration) * 1000000;
        const int64_t start_begin = logo_begin + static_cast<int64_t>(logo) * 1000000;
        int64_t next_disciplined = logo_begin;
        if (repeat_start != 0 && disciplined_now >= start_begin) {
            on = true;
            next_disciplined = epoch + (index + 1) * cycle;
        } else if (disciplined_now >= logo_begin) {
            const int64_t half = (disciplined_now - logo_begin) / kHalfSecond;
            on = (half % 2) == 0;
            next_disciplined = logo_begin + (half + 1) * kHalfSecond;
        }
        next_local = rtc_discipline_disciplined_to_local_us(next_disciplined);
    }
    WriteOutput(on);
    if (esp_timer_start_once(timer, static_cast<uint64_t>(std::max<int64_t>(1, next_local - local_now))) != ESP_OK) {
        active = false;
        WriteOutput(false);
    }
}

void OnTimer(void*) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    UpdateLocked();
    xSemaphoreGive(mutex);
}
}

bool Init() {
    if (ready) return true;
    mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr) return false;
    gpio_config_t pin{};
    pin.pin_bit_mask = 1ULL << 33;
    pin.mode = GPIO_MODE_OUTPUT;
    pin.pull_up_en = GPIO_PULLUP_DISABLE;
    pin.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pin.intr_type = GPIO_INTR_DISABLE;
    // Preload OFF before enabling the pin as an output to avoid an active-LOW
    // pulse during GPIO configuration.
    WriteOutput(false);
    if (gpio_config(&pin) != ESP_OK) { vSemaphoreDelete(mutex); mutex = nullptr; return false; }
    WriteOutput(false);
    esp_timer_create_args_t args{};
    args.callback = OnTimer;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "repeat_output";
    if (esp_timer_create(&args, &timer) != ESP_OK) { vSemaphoreDelete(mutex); mutex = nullptr; return false; }
    ready = true;
    return true;
}

bool Ready() { return ready; }

void Schedule(int64_t start_disciplined_us, uint32_t seconds, uint32_t count, uint32_t logo_seconds, uint32_t repeat_start_seconds) {
    if (!ready) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    active = logo_seconds == factory_timer::kRepetitionLogoSeconds && seconds > 0 && count > 0 &&
        (repeat_start_seconds == 0 || repeat_start_seconds == factory_timer::kRepetitionLogoSeconds);
    epoch = start_disciplined_us;
    duration = seconds; repeats = count; logo = logo_seconds; repeat_start = repeat_start_seconds;
    intro_end = esp_timer_get_time() + 4000000;
    UpdateLocked();
    xSemaphoreGive(mutex);
}

void Cancel() {
    if (!ready) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    active = false;
    UpdateLocked();
    xSemaphoreGive(mutex);
}
}
