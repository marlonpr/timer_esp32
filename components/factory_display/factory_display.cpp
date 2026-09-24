#include "factory_display.h"
#include "display_backend.h"
#include "logo_bitmap.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

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

struct DisplayCommand {
    enum class Type : uint8_t { Arm, Reset } type{Type::Reset};
    int64_t local_start_us{};
    uint32_t duration_seconds{};
};

QueueHandle_t s_command_queue = nullptr;
TaskHandle_t s_display_task = nullptr;
std::atomic<bool> s_ready{false};

void SetPresentationDiagnosticEdge(bool high) {
#if defined(CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_FACTORY_DISPLAY_EDGE_GPIO), high ? 1 : 0);
#else
    (void)high;
#endif
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
        SetPresentationDiagnosticEdge(false);

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

        const int64_t start_flip_request_us = esp_timer_get_time();
        factory_display_backend_flip();
        const int64_t start_flip_commit_us = esp_timer_get_time();
        // This diagnostic marks software presentation commit. On S3 the GDMA
        // backend returns only after the new descriptor chain is active. On
        // classic ESP32 it marks the front/back pointer swap; actual photons
        // may follow by up to roughly one software-scan frame.
        SetPresentationDiagnosticEdge(true);

        const int64_t request_lateness_us =
            start_flip_request_us - command.local_start_us;
        const int64_t commit_lateness_us =
            start_flip_commit_us - command.local_start_us;
        ESP_LOGI(kTag,
                 "Visual countdown started: duration=%u target_local_us=%lld flip_request_lateness_us=%lld flip_commit_lateness_us=%lld",
                 static_cast<unsigned>(command.duration_seconds),
                 static_cast<long long>(command.local_start_us),
                 static_cast<long long>(request_lateness_us),
                 static_cast<long long>(commit_lateness_us));

        int64_t worst_flip_lateness_us = commit_lateness_us;
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

            factory_display_backend_flip();
            const int64_t flip_commit_us = esp_timer_get_time();
            worst_flip_lateness_us = std::max(worst_flip_lateness_us,
                                               flip_commit_us - boundary_us);
        }

        if (!superseded) {
            ESP_LOGI(kTag,
                     "Visual countdown finished: duration=%u worst_flip_call_lateness_us=%lld",
                     static_cast<unsigned>(command.duration_seconds),
                     static_cast<long long>(worst_flip_lateness_us));
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
             "64x32 countdown display ready: idle=logo running=MM:SS scheduler_core=%d priority=%u",
             static_cast<int>(kDisplaySchedulerCore),
             static_cast<unsigned>(kDisplaySchedulerPriority));
    return true;
}

extern "C" void factory_display_set_brightness_percent(uint8_t brightness_percent) {
    if (!s_ready.load()) return;
    if (brightness_percent > 100) brightness_percent = 100;
    factory_display_backend_set_brightness_percent(brightness_percent);
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
