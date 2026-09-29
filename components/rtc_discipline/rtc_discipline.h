#ifndef RTC_DISCIPLINE_H
#define RTC_DISCIPLINE_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#include "ds3231.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTC_DISCIPLINE_UNINITIALIZED = 0,
    RTC_DISCIPLINE_ACQUIRING,
    RTC_DISCIPLINE_LOCKED,
    RTC_DISCIPLINE_HOLDOVER,
} rtc_discipline_state_t;

typedef struct {
    gpio_num_t sqw_gpio;
    bool enable_internal_pullup;
    uint16_t acquire_points;       /* default 33 points = 32 s */
    uint16_t fit_points;           /* default 129 points = 128 s */
    uint32_t holdover_timeout_ms;  /* default 3500 ms */
    uint32_t max_inferred_gap_s;   /* default 8 s */
} rtc_discipline_config_t;

#define RTC_DISCIPLINE_CONFIG_DEFAULT(_sqw_gpio) \
    {                                                \
        .sqw_gpio = (_sqw_gpio),                     \
        .enable_internal_pullup = true,              \
        .acquire_points = 33,                        \
        .fit_points = 129,                           \
        .holdover_timeout_ms = 3500,                 \
        .max_inferred_gap_s = 8,                     \
    }

typedef struct {
    rtc_discipline_state_t state;
    uint64_t accepted_edges;
    uint64_t rejected_edges;
    uint64_t inferred_missing_edges;
    uint32_t isr_queue_drops;
    int64_t last_edge_local_us;
    double local_us_per_rtc_second;
    double local_rate_ppm_vs_rtc;
    double fit_rms_us;
    uint16_t fit_point_count;
    float rtc_temperature_c;
    bool rtc_temperature_valid;
    bool osf_was_set;
    bool osf_cleared_after_edges;

    /* SQW timestamp-repeatability diagnostics. Statistics are over the most
     * recent accepted 1-Hz intervals (up to 64). They describe ESP-side
     * timestamp repeatability, not RTC frequency accuracy. */
    int32_t sqw_isr_core_id;
    uint32_t sqw_interval_samples;
    int64_t sqw_last_interval_us;
    double sqw_interval_mean_us;
    double sqw_interval_rms_jitter_us;
    double sqw_interval_p2p_us;
    uint32_t sqw_trace_samples;
    uint32_t sqw_trace_overwrites;
} rtc_discipline_status_t;

/**
 * Start DS3231 1-Hz capture and the background estimator.
 * Does not change the network/master START phase.
 */
esp_err_t rtc_discipline_init(ds3231_dev_t *rtc,
                              const rtc_discipline_config_t *config);

/** Current continuous disciplined time in microseconds. */
int64_t rtc_discipline_now_us(void);

/** Convert a raw esp_timer local timestamp to the current disciplined domain. */
int64_t rtc_discipline_local_to_disciplined_us(int64_t local_us);

/** Convert a disciplined deadline to the raw esp_timer domain using current slope. */
int64_t rtc_discipline_disciplined_to_local_us(int64_t disciplined_us);

/** Stable text name for logs and qualification tooling. */
const char *rtc_discipline_state_name(rtc_discipline_state_t state);

/** Snapshot estimator/health state. */
void rtc_discipline_get_status(rtc_discipline_status_t *out_status);

/** True only after enough valid SQW observations have produced a sane fit. */
bool rtc_discipline_is_locked(void);

/* v6.17 diagnostic-safe SQW trace. The ISR records only its already-captured
 * raw local timestamp plus core id. No refresh/network metadata is sampled and
 * no diagnostic GPIO is driven from the SQW path. */
void rtc_discipline_dump_sqw_trace(void);


#ifdef __cplusplus
}
#endif

#endif // RTC_DISCIPLINE_H
