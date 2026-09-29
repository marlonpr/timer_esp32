#include "display_backend.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_attr.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_struct.h"

#define PANEL_WIDTH   64
#define PANEL_HEIGHT  32
#define PHY_WIDTH     64
#define PHY_HEIGHT    32
#define COLOR_DEPTH   3
#define BASE_US       30

#define PIN_R1  GPIO_NUM_2
#define PIN_G1  GPIO_NUM_4
#define PIN_B1  GPIO_NUM_5
#define PIN_R2  GPIO_NUM_18
#define PIN_G2  GPIO_NUM_19
#define PIN_B2  GPIO_NUM_25
#define PIN_CLK GPIO_NUM_13
#define PIN_LAT GPIO_NUM_12
#define PIN_OE  GPIO_NUM_14
#define PIN_A   GPIO_NUM_15
#define PIN_B   GPIO_NUM_26
#define PIN_C   GPIO_NUM_23

#define BIT_R1 (1U << PIN_R1)
#define BIT_G1 (1U << PIN_G1)
#define BIT_B1 (1U << PIN_B1)
#define BIT_R2 (1U << PIN_R2)
#define BIT_G2 (1U << PIN_G2)
#define BIT_B2 (1U << PIN_B2)
#define BIT_A  (1U << PIN_A)
#define BIT_B  (1U << PIN_B)
#define BIT_C  (1U << PIN_C)
#define BIT_CLK (1U << PIN_CLK)
#define BIT_LAT (1U << PIN_LAT)

#define OE_DUTY_RES  LEDC_TIMER_6_BIT
#define OE_MAX_DUTY  63U
#define OE_FREQ_HZ   1000000
#define OE_SPEED_MODE LEDC_HIGH_SPEED_MODE
#define OE_CHANNEL    LEDC_CHANNEL_0

static const char *TAG = "factory_display_esp32";
static const BaseType_t REFRESH_TASK_CORE = 1;
static const UBaseType_t REFRESH_TASK_PRIORITY = 20;

static uint8_t framebuffers[2][COLOR_DEPTH][PHY_HEIGHT][PHY_WIDTH];
// The refresh loop consumes only this aligned 32-bit publication. The inactive
// frame is always index ^ 1 and is rendered completely before publication.
static volatile uint32_t active_frame_index = 0;
// Render routines call clear() first; that snapshots the inactive frame here so
// all subsequent pixel writes for one prepared frame target the same buffer.
static uint32_t render_frame_index = 1;
static volatile uint8_t global_brightness_pct = 50;
#if defined(CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS
static volatile uint8_t refresh_marker_level = 0;
#endif

static inline uint32_t load_active_frame_index(void) {
    return __atomic_load_n(&active_frame_index, __ATOMIC_ACQUIRE) & 1U;
}

static inline uint32_t inactive_frame_index(void) {
    return load_active_frame_index() ^ 1U;
}

static inline void IRAM_ATTR direct_gpio_write_level(int gpio_num, bool high) {
    if (gpio_num < 32) {
        const uint32_t mask = 1U << gpio_num;
        if (high) GPIO.out_w1ts = mask; else GPIO.out_w1tc = mask;
    } else {
        const uint32_t mask = 1U << (gpio_num - 32);
        if (high) GPIO.out1_w1ts.val = mask; else GPIO.out1_w1tc.val = mask;
    }
}


static void update_oe_duty(void) {
    const uint32_t duty = (global_brightness_pct * OE_MAX_DUTY) / 100U;
    ESP_ERROR_CHECK(ledc_set_duty(OE_SPEED_MODE, OE_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(OE_SPEED_MODE, OE_CHANNEL));
}

static void panel_set_pixel(int x, int y, uint8_t r8, uint8_t g8, uint8_t b8) {
    if ((unsigned)x >= PANEL_WIDTH || (unsigned)y >= PANEL_HEIGHT) return;

    // Full-intensity primary colors are all this countdown test needs. Linear
    // 8-bit -> 3-bit quantization keeps the original test driver's BCM format.
    const uint8_t r_q = r8 >> 5;
    const uint8_t g_q = g8 >> 5;
    const uint8_t b_q = b8 >> 5;

    for (int plane = 0; plane < COLOR_DEPTH; ++plane) {
        uint8_t value = 0;
        if ((r_q >> plane) & 1U) value |= 0x01;
        if ((g_q >> plane) & 1U) value |= 0x02;
        if ((b_q >> plane) & 1U) value |= 0x04;
        framebuffers[render_frame_index][plane][y][x] = value;
    }
}

static void refresh_task(void *arg) {
    (void)arg;

    // CPU1 is dedicated to software HUB75 refresh in the classic ESP32 profiles.
    // Its idle-task TWDT subscription is disabled in sdkconfig, so the refresh
    // loop does not need to call esp_task_wdt_reset() at frame rate. That avoids
    // a periodic control-path interruption in the scan loop.

    const int scan_rows = PANEL_HEIGHT / 4;  // 8 rows for the proven P5 wiring.
    const int total_cols = PANEL_WIDTH * 2;

    uint32_t refresh_frame_index = load_active_frame_index();
    uint32_t last_adopted_frame_index = refresh_frame_index;

    while (1) {
        refresh_frame_index = load_active_frame_index();
#if defined(CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS
        if (refresh_frame_index != last_adopted_frame_index) {
            refresh_marker_level ^= 1U;
            direct_gpio_write_level(CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_GPIO,
                                    refresh_marker_level != 0);
            last_adopted_frame_index = refresh_frame_index;
        }
#else
        last_adopted_frame_index = refresh_frame_index;
#endif

        for (int plane = 0; plane < COLOR_DEPTH; ++plane) {
            const int weight = 1 << plane;

            for (int row = 0; row < scan_rows; ++row) {
                // row address / shift preparation
                // Blank while changing address and shifting.
                ledc_set_duty(OE_SPEED_MODE, OE_CHANNEL, 0);
                ledc_update_duty(OE_SPEED_MODE, OE_CHANNEL);

                uint32_t set_mask = 0;
                uint32_t clr_mask = 0;
                if (row & 0x01) set_mask |= BIT_A; else clr_mask |= BIT_A;
                if (row & 0x02) set_mask |= BIT_B; else clr_mask |= BIT_B;
                if (row & 0x04) set_mask |= BIT_C; else clr_mask |= BIT_C;
                GPIO.out_w1ts = set_mask;
                GPIO.out_w1tc = clr_mask;

                for (int col = 0; col < total_cols; ++col) {
                    const int panel_index = col / PANEL_WIDTH;
                    const int local_x = col % PANEL_WIDTH;
                    const int y1 = (panel_index % 2) ? row : row + scan_rows;
                    const int y2 = y1 + 2 * scan_rows;

                    const uint8_t p1 = framebuffers[refresh_frame_index][plane][y1][local_x];
                    const uint8_t p2 = framebuffers[refresh_frame_index][plane][y2][local_x];

                    uint32_t set2 = 0;
                    uint32_t clr2 = 0;
                    if (p1 & 0x01) set2 |= BIT_R1; else clr2 |= BIT_R1;
                    if (p1 & 0x02) set2 |= BIT_G1; else clr2 |= BIT_G1;
                    if (p1 & 0x04) set2 |= BIT_B1; else clr2 |= BIT_B1;
                    if (p2 & 0x01) set2 |= BIT_R2; else clr2 |= BIT_R2;
                    if (p2 & 0x02) set2 |= BIT_G2; else clr2 |= BIT_G2;
                    if (p2 & 0x04) set2 |= BIT_B2; else clr2 |= BIT_B2;

                    GPIO.out_w1ts = set2;
                    GPIO.out_w1tc = clr2;
                    GPIO.out_w1ts = BIT_CLK;
                    GPIO.out_w1tc = BIT_CLK;
                }

                // latch edge
                GPIO.out_w1ts = BIT_LAT;
                GPIO.out_w1tc = BIT_LAT;
                update_oe_duty();

                // weighted OE dwell
                for (int t = 0; t < weight; ++t) {
                    esp_rom_delay_us(BASE_US);
                }
            }
        }

    }
}

bool factory_display_backend_init(uint8_t brightness) {
    active_frame_index = 0;
    render_frame_index = 1;
    memset(framebuffers, 0, sizeof(framebuffers));

    const uint64_t mask = (1ULL << PIN_R1) | (1ULL << PIN_G1) | (1ULL << PIN_B1) |
                          (1ULL << PIN_R2) | (1ULL << PIN_G2) | (1ULL << PIN_B2) |
                          (1ULL << PIN_A) | (1ULL << PIN_B) | (1ULL << PIN_C) |
                          (1ULL << PIN_CLK) | (1ULL << PIN_LAT);
    gpio_config_t io_conf = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    gpio_set_level(PIN_LAT, 0);
    gpio_set_level(PIN_CLK, 0);

#if defined(CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS
    gpio_config_t refresh_marker_conf = {
        .pin_bit_mask = 1ULL << CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&refresh_marker_conf));
    refresh_marker_level = 0;
    direct_gpio_write_level(CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_GPIO, false);
#endif

    ledc_timer_config_t timer_conf = {
        .speed_mode = OE_SPEED_MODE,
        .duty_resolution = OE_DUTY_RES,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = OE_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    ledc_channel_config_t channel_conf = {
        .gpio_num = PIN_OE,
        .speed_mode = OE_SPEED_MODE,
        .channel = OE_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {.output_invert = 1},
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_conf));

    global_brightness_pct = (uint8_t)(((uint32_t)brightness * 100U) / 255U);
    if (global_brightness_pct == 0 && brightness > 0) global_brightness_pct = 1;
    update_oe_duty();

    if (xTaskCreatePinnedToCore(refresh_task, "hub75_refresh", 3072, NULL,
                                REFRESH_TASK_PRIORITY, NULL, REFRESH_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Could not create software HUB75 refresh task");
        return false;
    }

    ESP_LOGI(TAG,
             "ESP32 P5 backend ready: 64x32 four-scan pins R1=2 G1=4 B1=5 R2=18 G2=19 B2=25 A=15 B=26 C=23 LAT=12 OE=14 CLK=13 brightness=%u%% refresh_core=%d refresh_priority=%u",
             (unsigned)global_brightness_pct, (int)REFRESH_TASK_CORE,
             (unsigned)REFRESH_TASK_PRIORITY);
#if defined(CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS
    ESP_LOGI(TAG, "Refresh-side frame-adoption marker enabled on GPIO%d",
             CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_GPIO);
#endif
    return true;
}

void factory_display_backend_set_brightness_percent(uint8_t brightness_percent) {
    if (brightness_percent > 100) brightness_percent = 100;
    global_brightness_pct = brightness_percent;
    update_oe_duty();
    ESP_LOGI(TAG, "Panel brightness set to %u%%", (unsigned)global_brightness_pct);
}

void factory_display_backend_clear(void) {
    render_frame_index = inactive_frame_index();
    memset(framebuffers[render_frame_index], 0, sizeof(framebuffers[render_frame_index]));
}

void factory_display_backend_fill_rect(int x, int y, int width, int height,
                                       uint8_t r, uint8_t g, uint8_t b) {
    for (int py = y; py < y + height; ++py) {
        for (int px = x; px < x + width; ++px) {
            panel_set_pixel(px, py, r, g, b);
        }
    }
}

void factory_display_backend_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    panel_set_pixel(x, y, r, g, b);
}

void factory_display_backend_flip(void) {
    const uint32_t next = inactive_frame_index();
    __atomic_store_n(&active_frame_index, next, __ATOMIC_RELEASE);
}

void IRAM_ATTR factory_display_backend_publish_prepared_from_isr(void) {
    // Rendering always targets the inactive frame. Publication is exactly one
    // aligned 32-bit store observed by the core-1 refresh loop at its next full
    // scan boundary. No pointer triplet can be seen half-swapped.
    const uint32_t current = __atomic_load_n(&active_frame_index, __ATOMIC_RELAXED) & 1U;
    __atomic_store_n(&active_frame_index, current ^ 1U, __ATOMIC_RELEASE);
}
