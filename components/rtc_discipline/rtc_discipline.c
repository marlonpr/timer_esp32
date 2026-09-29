#include "rtc_discipline.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define TAG "rtc_discipline"

#define NS_PER_SECOND 1000000000LL
#define US_PER_SECOND 1000000LL
#define MAX_FIT_POINTS 129
#define EDGE_QUEUE_DEPTH 16
#define RTC_TASK_STACK 4096
#define RTC_TASK_PRIORITY 8
#define SANE_RATE_LIMIT_PPM 500.0
#define GLITCH_REJECT_US 400000LL
#define GAP_RESIDUAL_LIMIT_US 150000.0
#define OSF_CLEAR_AFTER_VALID_EDGES 3
#define TEMP_REFRESH_ACCEPTED_EDGES 64

typedef struct {
    int64_t sequence;
    int64_t local_us;
} fit_point_t;

typedef struct {
    bool initialized;
    ds3231_dev_t *rtc;
    rtc_discipline_config_t cfg;
    QueueHandle_t edge_queue;
    TaskHandle_t task;
    int64_t init_local_us;

    portMUX_TYPE mux;
    int64_t anchor_local_us;
    int64_t anchor_disciplined_ns;
    int64_t local_rate_ppb_vs_rtc;

    fit_point_t points[MAX_FIT_POINTS];
    uint16_t point_count;
    uint16_t point_head;
    int64_t next_sequence;
    int64_t last_accepted_local_us;

    volatile uint32_t isr_queue_drops;
    rtc_discipline_status_t status;
} rtc_discipline_ctx_t;

static rtc_discipline_ctx_t s_ctx = {
    .mux = portMUX_INITIALIZER_UNLOCKED,
};

const char *rtc_discipline_state_name(rtc_discipline_state_t state)
{
    switch (state) {
        case RTC_DISCIPLINE_ACQUIRING: return "ACQUIRING";
        case RTC_DISCIPLINE_LOCKED: return "LOCKED";
        case RTC_DISCIPLINE_HOLDOVER: return "HOLDOVER";
        default: return "UNINITIALIZED";
    }
}

static int64_t scale_i64_ratio_rounded(int64_t value,
                                       int64_t numerator,
                                       int64_t denominator)
{
    if (denominator <= 0 || numerator <= 0) {
        return value;
    }

    /*
     * Avoid __int128 so this stays portable to 32-bit Xtensa. Splitting the
     * input by denominator keeps remainder*numerator below roughly 1e18 for
     * the +/-500 ppm rates accepted here. Round the sub-unit remainder instead
     * of truncating it so repeated reslopes cannot accumulate a one-sided bias.
     */
    const int64_t quotient = value / denominator;
    const int64_t remainder = value % denominator;
    const int64_t remainder_product = remainder * numerator;
    int64_t scaled_remainder = remainder_product / denominator;
    const int64_t residual = remainder_product % denominator;

    if (residual >= (denominator + 1) / 2) {
        scaled_remainder += 1;
    } else if (residual <= -((denominator + 1) / 2)) {
        scaled_remainder -= 1;
    }

    return quotient * numerator + scaled_remainder;
}

static int64_t ns_to_us_rounded(int64_t nanoseconds)
{
    const int64_t whole_us = nanoseconds / 1000LL;
    const int64_t remainder_ns = nanoseconds % 1000LL;
    if (remainder_ns >= 500LL) {
        return whole_us + 1LL;
    }
    if (remainder_ns <= -500LL) {
        return whole_us - 1LL;
    }
    return whole_us;
}

static int64_t apply_local_to_disciplined_delta_ns(int64_t local_delta_us,
                                                    int64_t local_rate_ppb)
{
    const int64_t denominator = NS_PER_SECOND + local_rate_ppb;
    const int64_t local_delta_ns = local_delta_us * 1000LL;
    return scale_i64_ratio_rounded(local_delta_ns, NS_PER_SECOND, denominator);
}

static int64_t apply_disciplined_to_local_delta_ns(int64_t disciplined_delta_ns,
                                                    int64_t local_rate_ppb)
{
    const int64_t factor = NS_PER_SECOND + local_rate_ppb;
    return scale_i64_ratio_rounded(disciplined_delta_ns, factor, NS_PER_SECOND);
}

static int64_t local_to_disciplined_ns_locked(int64_t local_us)
{
    const int64_t delta_us = local_us - s_ctx.anchor_local_us;
    return s_ctx.anchor_disciplined_ns +
           apply_local_to_disciplined_delta_ns(delta_us,
                                                s_ctx.local_rate_ppb_vs_rtc);
}

static int64_t local_to_disciplined_locked(int64_t local_us)
{
    return ns_to_us_rounded(local_to_disciplined_ns_locked(local_us));
}

int64_t rtc_discipline_local_to_disciplined_us(int64_t local_us)
{
    if (!s_ctx.initialized) {
        return local_us;
    }

    taskENTER_CRITICAL(&s_ctx.mux);
    const int64_t result = local_to_disciplined_locked(local_us);
    taskEXIT_CRITICAL(&s_ctx.mux);
    return result;
}

int64_t rtc_discipline_now_us(void)
{
    return rtc_discipline_local_to_disciplined_us(esp_timer_get_time());
}

int64_t rtc_discipline_disciplined_to_local_us(int64_t disciplined_us)
{
    if (!s_ctx.initialized) {
        return disciplined_us;
    }

    taskENTER_CRITICAL(&s_ctx.mux);
    const int64_t disciplined_delta_ns =
        disciplined_us * 1000LL - s_ctx.anchor_disciplined_ns;
    const int64_t local_delta_ns =
        apply_disciplined_to_local_delta_ns(disciplined_delta_ns,
                                             s_ctx.local_rate_ppb_vs_rtc);
    const int64_t result =
        s_ctx.anchor_local_us + ns_to_us_rounded(local_delta_ns);
    taskEXIT_CRITICAL(&s_ctx.mux);
    return result;
}

static void set_state(rtc_discipline_state_t new_state)
{
    rtc_discipline_state_t old_state;

    taskENTER_CRITICAL(&s_ctx.mux);
    old_state = s_ctx.status.state;
    s_ctx.status.state = new_state;
    taskEXIT_CRITICAL(&s_ctx.mux);

    if (old_state != new_state) {
        ESP_LOGW(TAG, "State %s -> %s", rtc_discipline_state_name(old_state), rtc_discipline_state_name(new_state));
    }
}

static void install_rate_without_phase_jump(int64_t local_now_us,
                                            double local_us_per_rtc_second,
                                            double fit_rms_us)
{
    const double ppm = local_us_per_rtc_second - 1000000.0;
    const int64_t new_rate_ppb = (int64_t)llround(ppm * 1000.0);

    taskENTER_CRITICAL(&s_ctx.mux);
    /*
     * Preserve the phase anchor below one microsecond. The previous code
     * converted disciplined_now to integer microseconds before re-anchoring on
     * every SQW edge. A stable fractional microsecond was therefore discarded
     * once per second and accumulated as an artificial 0..1 ppm clock error.
     * Keeping the anchor in nanoseconds makes reslope quantization sub-ns.
     */
    const int64_t disciplined_now_ns =
        local_to_disciplined_ns_locked(local_now_us);
    s_ctx.anchor_local_us = local_now_us;
    s_ctx.anchor_disciplined_ns = disciplined_now_ns;
    s_ctx.local_rate_ppb_vs_rtc = new_rate_ppb;
    s_ctx.status.local_us_per_rtc_second = local_us_per_rtc_second;
    s_ctx.status.local_rate_ppm_vs_rtc = ppm;
    s_ctx.status.fit_rms_us = fit_rms_us;
    taskEXIT_CRITICAL(&s_ctx.mux);
}

static void reset_fit(void)
{
    s_ctx.point_count = 0;
    s_ctx.point_head = 0;
    s_ctx.next_sequence = 0;
    s_ctx.last_accepted_local_us = 0;

    taskENTER_CRITICAL(&s_ctx.mux);
    s_ctx.status.fit_point_count = 0;
    taskEXIT_CRITICAL(&s_ctx.mux);
}

static void add_fit_point(int64_t sequence, int64_t local_us)
{
    const uint16_t capacity = s_ctx.cfg.fit_points;

    if (s_ctx.point_count < capacity) {
        const uint16_t index = (uint16_t)((s_ctx.point_head + s_ctx.point_count) % capacity);
        s_ctx.points[index] = (fit_point_t){.sequence = sequence, .local_us = local_us};
        s_ctx.point_count++;
    } else {
        s_ctx.points[s_ctx.point_head] = (fit_point_t){.sequence = sequence, .local_us = local_us};
        s_ctx.point_head = (uint16_t)((s_ctx.point_head + 1u) % capacity);
    }

    taskENTER_CRITICAL(&s_ctx.mux);
    s_ctx.status.fit_point_count = s_ctx.point_count;
    taskEXIT_CRITICAL(&s_ctx.mux);
}

static bool fit_rate(double *slope_us_per_second, double *rms_us)
{
    if (!slope_us_per_second || !rms_us || s_ctx.point_count < s_ctx.cfg.acquire_points) {
        return false;
    }

    const fit_point_t *first = &s_ctx.points[s_ctx.point_head];
    const double x0 = (double)first->sequence;
    const double y0 = (double)first->local_us;

    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;

    for (uint16_t i = 0; i < s_ctx.point_count; ++i) {
        const uint16_t index = (uint16_t)((s_ctx.point_head + i) % s_ctx.cfg.fit_points);
        const double x = (double)s_ctx.points[index].sequence - x0;
        const double y = (double)s_ctx.points[index].local_us - y0;
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }

    const double n = (double)s_ctx.point_count;
    const double denominator = n * sum_xx - sum_x * sum_x;
    if (fabs(denominator) < 1e-9) {
        return false;
    }

    const double slope = (n * sum_xy - sum_x * sum_y) / denominator;
    const double intercept = (sum_y - slope * sum_x) / n;
    const double ppm = slope - 1000000.0;

    if (!isfinite(slope) || fabs(ppm) > SANE_RATE_LIMIT_PPM) {
        return false;
    }

    double residual_sq = 0.0;
    for (uint16_t i = 0; i < s_ctx.point_count; ++i) {
        const uint16_t index = (uint16_t)((s_ctx.point_head + i) % s_ctx.cfg.fit_points);
        const double x = (double)s_ctx.points[index].sequence - x0;
        const double y = (double)s_ctx.points[index].local_us - y0;
        const double residual = y - (intercept + slope * x);
        residual_sq += residual * residual;
    }

    *slope_us_per_second = slope;
    *rms_us = sqrt(residual_sq / n);
    return true;
}

static void refresh_temperature(void)
{
    float temperature_c = 0.0f;
    if (ds3231_get_temperature_c(s_ctx.rtc, &temperature_c) == ESP_OK) {
        taskENTER_CRITICAL(&s_ctx.mux);
        s_ctx.status.rtc_temperature_c = temperature_c;
        s_ctx.status.rtc_temperature_valid = true;
        taskEXIT_CRITICAL(&s_ctx.mux);
    }
}

static void maybe_clear_osf_after_sane_edges(void)
{
    bool should_clear = false;

    taskENTER_CRITICAL(&s_ctx.mux);
    should_clear = s_ctx.status.osf_was_set &&
                   !s_ctx.status.osf_cleared_after_edges &&
                   s_ctx.status.accepted_edges >= OSF_CLEAR_AFTER_VALID_EDGES;
    taskEXIT_CRITICAL(&s_ctx.mux);

    if (!should_clear) {
        return;
    }

    const esp_err_t err = ds3231_clear_oscillator_stop_flag(s_ctx.rtc);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_ctx.mux);
        s_ctx.status.osf_cleared_after_edges = true;
        taskEXIT_CRITICAL(&s_ctx.mux);
        ESP_LOGI(TAG, "OSF cleared after %u sane SQW edges", OSF_CLEAR_AFTER_VALID_EDGES);
    } else {
        ESP_LOGW(TAG, "Failed to clear OSF: %s", esp_err_to_name(err));
    }
}

static bool accept_edge(int64_t local_us)
{
    if (s_ctx.last_accepted_local_us == 0) {
        s_ctx.next_sequence = 0;
        s_ctx.last_accepted_local_us = local_us;
        add_fit_point(s_ctx.next_sequence, local_us);
        s_ctx.next_sequence++;
        return true;
    }

    const int64_t delta_us = local_us - s_ctx.last_accepted_local_us;
    if (delta_us < GLITCH_REJECT_US) {
        return false;
    }

    double expected_us;
    taskENTER_CRITICAL(&s_ctx.mux);
    expected_us = s_ctx.status.local_us_per_rtc_second;
    taskEXIT_CRITICAL(&s_ctx.mux);

    if (expected_us < 900000.0 || expected_us > 1100000.0) {
        expected_us = 1000000.0;
    }

    int64_t span = (int64_t)llround((double)delta_us / expected_us);
    if (span < 1 || span > (int64_t)s_ctx.cfg.max_inferred_gap_s) {
        return false;
    }

    const double residual_us = fabs((double)delta_us - ((double)span * expected_us));
    if (residual_us > GAP_RESIDUAL_LIMIT_US) {
        return false;
    }

    if (span > 1) {
        taskENTER_CRITICAL(&s_ctx.mux);
        s_ctx.status.inferred_missing_edges += (uint64_t)(span - 1);
        taskEXIT_CRITICAL(&s_ctx.mux);
    }

    s_ctx.next_sequence += (span - 1);
    add_fit_point(s_ctx.next_sequence, local_us);
    s_ctx.next_sequence++;
    s_ctx.last_accepted_local_us = local_us;
    return true;
}

static void process_edge(int64_t local_us)
{
    if (!accept_edge(local_us)) {
        taskENTER_CRITICAL(&s_ctx.mux);
        s_ctx.status.rejected_edges++;
        taskEXIT_CRITICAL(&s_ctx.mux);
        return;
    }

    uint64_t accepted_edges;
    taskENTER_CRITICAL(&s_ctx.mux);
    s_ctx.status.accepted_edges++;
    s_ctx.status.last_edge_local_us = local_us;
    accepted_edges = s_ctx.status.accepted_edges;
    taskEXIT_CRITICAL(&s_ctx.mux);

    maybe_clear_osf_after_sane_edges();

    if ((accepted_edges % TEMP_REFRESH_ACCEPTED_EDGES) == 1u) {
        refresh_temperature();
    }

    double slope = 0.0;
    double rms = 0.0;
    if (fit_rate(&slope, &rms)) {
        install_rate_without_phase_jump(local_us, slope, rms);
        set_state(RTC_DISCIPLINE_LOCKED);

#if !defined(CONFIG_FACTORY_RTC_QUAL_LOGS) || !CONFIG_FACTORY_RTC_QUAL_LOGS
        if ((accepted_edges % 16u) == 0u) {
            rtc_discipline_status_t status;
            rtc_discipline_get_status(&status);
            ESP_LOGI(TAG,
                     "RTC rate locked: local_us_per_s=%.3f rate_ppm=%+.3f rms_us=%.1f points=%u temp=%.2fC",
                     status.local_us_per_rtc_second,
                     status.local_rate_ppm_vs_rtc,
                     status.fit_rms_us,
                     (unsigned)status.fit_point_count,
                     status.rtc_temperature_valid ? status.rtc_temperature_c : 0.0f);
        }
#endif
    }
}

static void sqw_isr(void *arg)
{
    (void)arg;
    const int64_t timestamp_us = esp_timer_get_time();
    BaseType_t higher_priority_task_woken = pdFALSE;

    if (xQueueSendFromISR(s_ctx.edge_queue, &timestamp_us, &higher_priority_task_woken) != pdTRUE) {
        s_ctx.isr_queue_drops++;
    }

    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void discipline_task(void *arg)
{
    (void)arg;

    for (;;) {
        int64_t edge_us = 0;
        if (xQueueReceive(s_ctx.edge_queue, &edge_us, pdMS_TO_TICKS(500)) == pdTRUE) {
            rtc_discipline_state_t state;
            taskENTER_CRITICAL(&s_ctx.mux);
            state = s_ctx.status.state;
            taskEXIT_CRITICAL(&s_ctx.mux);

            if (state == RTC_DISCIPLINE_HOLDOVER) {
                /* Reacquire from fresh points, but retain the last good slope. */
                reset_fit();
                set_state(RTC_DISCIPLINE_ACQUIRING);
            }

            process_edge(edge_us);
            continue;
        }

        int64_t last_edge_us;
        rtc_discipline_state_t state;
        taskENTER_CRITICAL(&s_ctx.mux);
        last_edge_us = s_ctx.status.last_edge_local_us;
        state = s_ctx.status.state;
        s_ctx.status.isr_queue_drops = s_ctx.isr_queue_drops;
        taskEXIT_CRITICAL(&s_ctx.mux);

        if (state != RTC_DISCIPLINE_HOLDOVER) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t reference_us =
                last_edge_us > 0 ? last_edge_us : s_ctx.init_local_us;
            if ((now_us - reference_us) >
                ((int64_t)s_ctx.cfg.holdover_timeout_ms * 1000LL)) {
                set_state(RTC_DISCIPLINE_HOLDOVER);

                rtc_discipline_status_t status;
                rtc_discipline_get_status(&status);
                ESP_LOGW(TAG,
                         "RTC SQW absent/lost; holdover using last slope rate_ppm=%+.3f accepted_edges=%llu",
                         status.local_rate_ppm_vs_rtc,
                         (unsigned long long)status.accepted_edges);
            }
        }
    }
}

esp_err_t rtc_discipline_init(ds3231_dev_t *rtc,
                              const rtc_discipline_config_t *config)
{
    if (!rtc || !config || !GPIO_IS_VALID_GPIO(config->sqw_gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config->acquire_points < 3 ||
        config->fit_points < config->acquire_points ||
        config->fit_points > MAX_FIT_POINTS ||
        config->max_inferred_gap_s == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ctx.rtc = rtc;
    s_ctx.cfg = *config;
    reset_fit();

    const int64_t now_us = esp_timer_get_time();
    s_ctx.init_local_us = now_us;
    s_ctx.anchor_local_us = now_us;
    s_ctx.anchor_disciplined_ns = now_us * 1000LL;
    s_ctx.local_rate_ppb_vs_rtc = 0;
    s_ctx.status = (rtc_discipline_status_t){
        .state = RTC_DISCIPLINE_ACQUIRING,
        .local_us_per_rtc_second = 1000000.0,
        .local_rate_ppm_vs_rtc = 0.0,
    };

    esp_err_t err = ds3231_configure_1hz_sqw(rtc);
    if (err != ESP_OK) {
        return err;
    }

    bool osf = false;
    err = ds3231_get_oscillator_stop_flag(rtc, &osf);
    if (err != ESP_OK) {
        return err;
    }
    s_ctx.status.osf_was_set = osf;
    if (osf) {
        ESP_LOGW(TAG, "DS3231 OSF is set; waiting for sane SQW edges before clearing it");
    }

    refresh_temperature();

    s_ctx.edge_queue = xQueueCreate(EDGE_QUEUE_DEPTH, sizeof(int64_t));
    if (!s_ctx.edge_queue) {
        return ESP_ERR_NO_MEM;
    }

    const gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << config->sqw_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = config->enable_internal_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    err = gpio_config(&gpio_cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = gpio_isr_handler_add(config->sqw_gpio, sqw_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(discipline_task,
                    "rtc_discipline",
                    RTC_TASK_STACK,
                    NULL,
                    RTC_TASK_PRIORITY,
                    &s_ctx.task) != pdPASS) {
        gpio_isr_handler_remove(config->sqw_gpio);
        return ESP_ERR_NO_MEM;
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG,
             "Started: SQW GPIO=%d acquire_points=%u fit_points=%u holdover_ms=%u",
             config->sqw_gpio,
             (unsigned)config->acquire_points,
             (unsigned)config->fit_points,
             (unsigned)config->holdover_timeout_ms);
    return ESP_OK;
}

void rtc_discipline_get_status(rtc_discipline_status_t *out_status)
{
    if (!out_status) {
        return;
    }

    taskENTER_CRITICAL(&s_ctx.mux);
    *out_status = s_ctx.status;
    out_status->isr_queue_drops = s_ctx.isr_queue_drops;
    taskEXIT_CRITICAL(&s_ctx.mux);
}

bool rtc_discipline_is_locked(void)
{
    rtc_discipline_state_t state;
    taskENTER_CRITICAL(&s_ctx.mux);
    state = s_ctx.status.state;
    taskEXIT_CRITICAL(&s_ctx.mux);
    return state == RTC_DISCIPLINE_LOCKED;
}
