#include "factory_display.h"
#include "display_backend.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {

constexpr char kTag[] = "factory_display";
constexpr BaseType_t kDisplaySchedulerCore = 0;
constexpr UBaseType_t kDisplaySchedulerPriority = 12;
constexpr int64_t kFineLeadUs = 2000;
constexpr int64_t kTickUs = 1000000LL / configTICK_RATE_HZ;
constexpr uint32_t kMaxDisplayedSeconds = 99;

struct DisplayCommand {
    enum class Type : uint8_t { Arm, Reset } type{Type::Reset};
    int64_t local_start_us{};
    uint32_t duration_seconds{};
};

QueueHandle_t s_command_queue = nullptr;
TaskHandle_t s_display_task = nullptr;
uint8_t s_idle_value = 0;
std::atomic<bool> s_ready{false};

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

void DrawDigit(int origin_x, uint8_t digit, uint8_t r, uint8_t g, uint8_t b) {
    if (digit > 9) return;

    constexpr int y = 1;
    constexpr int thickness = 4;
    constexpr int horizontal_length = 14;
    constexpr int right_x = 18;
    constexpr int upper_vertical_y = 4;
    constexpr int lower_vertical_y = 17;
    constexpr int vertical_height = 9;
    constexpr int middle_y = 13;
    constexpr int bottom_y = 26;

    const uint8_t mask = kDigitSegments[digit];

    // Digit 1 has no horizontal segments, so the normal seven-segment geometry
    // would leave it visibly shorter than 0 (top blank 4 px / bottom blank 6 px).
    // Extend only its two right-hand segments so its visible bounding box is
    // y=1..29, exactly the same height as digit 0, while keeping the center gap.
    if (digit == 1) {
        constexpr int one_upper_y = 1;
        constexpr int one_upper_height = 12;  // y=1..12
        constexpr int one_lower_y = 17;
        constexpr int one_lower_height = 13;  // y=17..29
        Fill(origin_x + right_x, one_upper_y, thickness, one_upper_height, r, g, b);
        Fill(origin_x + right_x, one_lower_y, thickness, one_lower_height, r, g, b);
        return;
    }

    if (mask & kA) Fill(origin_x + 4, y, horizontal_length, thickness, r, g, b);
    if (mask & kB) Fill(origin_x + right_x, upper_vertical_y, thickness, vertical_height, r, g, b);
    if (mask & kC) Fill(origin_x + right_x, lower_vertical_y, thickness, vertical_height, r, g, b);
    if (mask & kD) Fill(origin_x + 4, bottom_y, horizontal_length, thickness, r, g, b);
    if (mask & kE) Fill(origin_x, lower_vertical_y, thickness, vertical_height, r, g, b);
    if (mask & kF) Fill(origin_x, upper_vertical_y, thickness, vertical_height, r, g, b);
    if (mask & kG) Fill(origin_x + 4, middle_y, horizontal_length, thickness, r, g, b);
}

void RenderTwoDigits(uint32_t value, uint8_t r, uint8_t g, uint8_t b) {
    value = std::min<uint32_t>(value, kMaxDisplayedSeconds);
    factory_display_backend_clear();

    constexpr int left_x = 7;
    constexpr int right_x = 35;
    DrawDigit(left_x, static_cast<uint8_t>((value / 10) % 10), r, g, b);
    DrawDigit(right_x, static_cast<uint8_t>(value % 10), r, g, b);
}

void RenderIdleToBackBuffer() {
    // Device identity in dim blue: ESP01 -> 01, ... ESP05 -> 05.
    RenderTwoDigits(s_idle_value, 0, 64, 255);
}

void RenderRunningToBackBuffer(uint32_t remaining_seconds) {
    RenderTwoDigits(remaining_seconds, 0, 255, 0);
}

void RenderFinishedToBackBuffer() {
    RenderTwoDigits(0, 255, 0, 0);
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
            RenderIdleToBackBuffer();
            factory_display_backend_flip();
            continue;
        }

        if (command.duration_seconds > kMaxDisplayedSeconds) {
            ESP_LOGW(kTag,
                     "Display is two-digit for this test; duration=%u will show 99 until remaining <=99",
                     static_cast<unsigned>(command.duration_seconds));
        }

        // Make the idle/device ID frame visible while armed, then pre-render
        // the first countdown frame. Nothing visible changes until deadline.
        RenderIdleToBackBuffer();
        factory_display_backend_flip();
        RenderRunningToBackBuffer(command.duration_seconds);

        DisplayCommand replacement{};
        if (!WaitUntilOrReplacement(command.local_start_us, &replacement)) {
            command = replacement;
            xQueueOverwrite(s_command_queue, &command);
            continue;
        }

        const int64_t start_flip_us = esp_timer_get_time();
        factory_display_backend_flip();
        const int64_t start_lateness_us = start_flip_us - command.local_start_us;
        ESP_LOGI(kTag,
                 "Visual countdown started: duration=%u target_local_us=%lld flip_call_lateness_us=%lld",
                 static_cast<unsigned>(command.duration_seconds),
                 static_cast<long long>(command.local_start_us),
                 static_cast<long long>(start_lateness_us));

        int64_t worst_flip_lateness_us = start_lateness_us;
        bool superseded = false;

        for (uint32_t elapsed = 1; elapsed <= command.duration_seconds; ++elapsed) {
            const uint32_t remaining = command.duration_seconds - elapsed;
            if (remaining == 0) {
                RenderFinishedToBackBuffer();
            } else {
                RenderRunningToBackBuffer(remaining);
            }

            const int64_t boundary_us =
                command.local_start_us + static_cast<int64_t>(elapsed) * 1000000LL;
            if (!WaitUntilOrReplacement(boundary_us, &replacement)) {
                command = replacement;
                xQueueOverwrite(s_command_queue, &command);
                superseded = true;
                break;
            }

            const int64_t flip_us = esp_timer_get_time();
            factory_display_backend_flip();
            worst_flip_lateness_us = std::max(worst_flip_lateness_us,
                                               flip_us - boundary_us);
        }

        if (!superseded) {
            ESP_LOGI(kTag,
                     "Visual countdown finished: duration=%u worst_flip_call_lateness_us=%lld",
                     static_cast<unsigned>(command.duration_seconds),
                     static_cast<long long>(worst_flip_lateness_us));
        }
    }
}

uint8_t ParseDeviceIdleValue(const char* device_id) {
    if (device_id == nullptr) return 0;
    const size_t length = std::strlen(device_id);
    if (length >= 2 && std::isdigit(static_cast<unsigned char>(device_id[length - 2])) &&
        std::isdigit(static_cast<unsigned char>(device_id[length - 1]))) {
        return static_cast<uint8_t>((device_id[length - 2] - '0') * 10 +
                                    (device_id[length - 1] - '0'));
    }
    return 0;
}

}  // namespace

extern "C" bool factory_display_init(const char* device_id, uint8_t brightness) {
    if (s_ready.load()) return true;

    s_idle_value = ParseDeviceIdleValue(device_id);
    if (!factory_display_backend_init(brightness)) {
        ESP_LOGE(kTag, "HUB75 backend initialization failed");
        return false;
    }

    s_command_queue = xQueueCreate(1, sizeof(DisplayCommand));
    if (s_command_queue == nullptr) {
        ESP_LOGE(kTag, "Could not create display command queue");
        return false;
    }

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
    ESP_LOGI(kTag,
             "64x32 countdown display ready: idle=%02u scheduler_core=%d priority=%u",
             static_cast<unsigned>(s_idle_value),
             static_cast<int>(kDisplaySchedulerCore),
             static_cast<unsigned>(kDisplaySchedulerPriority));
    return true;
}

extern "C" void factory_display_arm(int64_t local_start_us,
                                      uint32_t duration_seconds) {
    if (!s_ready.load() || s_command_queue == nullptr) return;
    DisplayCommand command{};
    command.type = DisplayCommand::Type::Arm;
    command.local_start_us = local_start_us;
    command.duration_seconds = duration_seconds;
    xQueueOverwrite(s_command_queue, &command);
}

extern "C" void factory_display_reset(void) {
    if (!s_ready.load() || s_command_queue == nullptr) return;
    DisplayCommand command{};
    command.type = DisplayCommand::Type::Reset;
    xQueueOverwrite(s_command_queue, &command);
}
