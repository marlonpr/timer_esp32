#ifndef DS3231_H
#define DS3231_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS3231_SDA_PIN 21
#define DS3231_SCL_PIN 22
#define DS3231_I2C_PORT I2C_NUM_0
#define DS3231_I2C_ADDR 0x68
#define DS3231_I2C_SPEED_HZ 100000

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t day_of_week;
} ds3231_time_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
} ds3231_dev_t;

/** Initialize DS3231 on GPIO21/GPIO22 using ESP-IDF i2c_master API. */
esp_err_t init_ds3231(ds3231_dev_t *out_dev);

/** Existing calendar helpers retained for compatibility. */
esp_err_t ds3231_set_time(ds3231_dev_t *dev, const ds3231_time_t *time);
esp_err_t ds3231_get_time(ds3231_dev_t *dev, ds3231_time_t *time);

/** Configure INT/SQW as a 1 Hz square-wave output while VCC is present. */
esp_err_t ds3231_configure_1hz_sqw(ds3231_dev_t *dev);

/** Read the oscillator-stop flag (OSF, status bit 7). */
esp_err_t ds3231_get_oscillator_stop_flag(ds3231_dev_t *dev, bool *is_set);

/** Clear OSF after the firmware has observed sane SQW edges. */
esp_err_t ds3231_clear_oscillator_stop_flag(ds3231_dev_t *dev);

/** Read the DS3231 temperature register in degrees Celsius (0.25 C resolution). */
esp_err_t ds3231_get_temperature_c(ds3231_dev_t *dev, float *temperature_c);

#ifdef __cplusplus
}
#endif

#endif // DS3231_H
