#include "ds3231.h"

#include <stddef.h>

#include "esp_log.h"

#define TAG "DS3231"

#define DS3231_REG_SECONDS      0x00
#define DS3231_REG_CONTROL      0x0E
#define DS3231_REG_STATUS       0x0F
#define DS3231_REG_TEMP_MSB     0x11

#define DS3231_CTRL_EOSC        (1u << 7)
#define DS3231_CTRL_BBSQW       (1u << 6)
#define DS3231_CTRL_RS2         (1u << 4)
#define DS3231_CTRL_RS1         (1u << 3)
#define DS3231_CTRL_INTCN       (1u << 2)
#define DS3231_CTRL_A2IE        (1u << 1)
#define DS3231_CTRL_A1IE        (1u << 0)

#define DS3231_STATUS_OSF       (1u << 7)

static inline uint8_t dec2bcd(uint8_t val)
{
    return (uint8_t)(((val / 10u) << 4) | (val % 10u));
}

static inline uint8_t bcd2dec(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10u) + (val & 0x0Fu));
}

static esp_err_t ds3231_read_regs(ds3231_dev_t *dev,
                                  uint8_t start_reg,
                                  uint8_t *data,
                                  size_t len)
{
    if (!dev || !dev->dev || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit_receive(dev->dev,
                                       &start_reg,
                                       1,
                                       data,
                                       len,
                                       -1);
}

static esp_err_t ds3231_write_reg(ds3231_dev_t *dev, uint8_t reg, uint8_t value)
{
    if (!dev || !dev->dev) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t data[2] = {reg, value};
    return i2c_master_transmit(dev->dev, data, sizeof(data), -1);
}

esp_err_t init_ds3231(ds3231_dev_t *out_dev)
{
    if (!out_dev) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_dev = (ds3231_dev_t){0};

    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = DS3231_I2C_PORT,
        .sda_io_num = DS3231_SDA_PIN,
        .scl_io_num = DS3231_SCL_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &out_dev->bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(err));
        return err;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_I2C_ADDR,
        .scl_speed_hz = DS3231_I2C_SPEED_HZ,
    };

    err = i2c_master_bus_add_device(out_dev->bus, &dev_cfg, &out_dev->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add DS3231: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "DS3231 initialized on SDA=%d, SCL=%d at %u Hz",
             DS3231_SDA_PIN,
             DS3231_SCL_PIN,
             (unsigned)DS3231_I2C_SPEED_HZ);
    return ESP_OK;
}

esp_err_t ds3231_set_time(ds3231_dev_t *dev, const ds3231_time_t *time)
{
    if (!dev || !dev->dev || !time) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[8];
    buf[0] = DS3231_REG_SECONDS;
    buf[1] = dec2bcd(time->second);
    buf[2] = dec2bcd(time->minute);
    buf[3] = dec2bcd((uint8_t)(time->hour & 0x3Fu));
    buf[4] = dec2bcd((uint8_t)(time->day_of_week & 0x07u));
    buf[5] = dec2bcd(time->day);
    buf[6] = dec2bcd(time->month);
    buf[7] = dec2bcd((uint8_t)(time->year % 100u));

    return i2c_master_transmit(dev->dev, buf, sizeof(buf), -1);
}

esp_err_t ds3231_get_time(ds3231_dev_t *dev, ds3231_time_t *time)
{
    if (!dev || !time) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[7];
    esp_err_t err = ds3231_read_regs(dev, DS3231_REG_SECONDS, data, sizeof(data));
    if (err != ESP_OK) {
        return err;
    }

    time->second = bcd2dec((uint8_t)(data[0] & 0x7Fu));
    time->minute = bcd2dec((uint8_t)(data[1] & 0x7Fu));
    time->hour = bcd2dec((uint8_t)(data[2] & 0x3Fu));
    time->day_of_week = bcd2dec((uint8_t)(data[3] & 0x07u));
    time->day = bcd2dec((uint8_t)(data[4] & 0x3Fu));
    time->month = bcd2dec((uint8_t)(data[5] & 0x1Fu));
    time->year = (uint16_t)(2000u + bcd2dec(data[6]));

    return ESP_OK;
}

esp_err_t ds3231_configure_1hz_sqw(ds3231_dev_t *dev)
{
    if (!dev || !dev->dev) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t control = 0;
    esp_err_t err = ds3231_read_regs(dev, DS3231_REG_CONTROL, &control, 1);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * EOSC=0: oscillator enabled when on backup supply as well.
     * BBSQW=0: no battery-backed SQW needed for this application.
     * RS2:RS1=00: 1 Hz.
     * INTCN=0: INT/SQW pin outputs square wave.
     * Alarm interrupt enables are cleared because INT/SQW is dedicated to SQW.
     */
    control &= (uint8_t)~(DS3231_CTRL_EOSC |
                          DS3231_CTRL_BBSQW |
                          DS3231_CTRL_RS2 |
                          DS3231_CTRL_RS1 |
                          DS3231_CTRL_INTCN |
                          DS3231_CTRL_A2IE |
                          DS3231_CTRL_A1IE);

    err = ds3231_write_reg(dev, DS3231_REG_CONTROL, control);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "INT/SQW configured for 1 Hz output");
    }
    return err;
}

esp_err_t ds3231_get_oscillator_stop_flag(ds3231_dev_t *dev, bool *is_set)
{
    if (!dev || !is_set) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status = 0;
    esp_err_t err = ds3231_read_regs(dev, DS3231_REG_STATUS, &status, 1);
    if (err != ESP_OK) {
        return err;
    }

    *is_set = (status & DS3231_STATUS_OSF) != 0;
    return ESP_OK;
}

esp_err_t ds3231_clear_oscillator_stop_flag(ds3231_dev_t *dev)
{
    if (!dev || !dev->dev) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status = 0;
    esp_err_t err = ds3231_read_regs(dev, DS3231_REG_STATUS, &status, 1);
    if (err != ESP_OK) {
        return err;
    }

    status &= (uint8_t)~DS3231_STATUS_OSF;
    return ds3231_write_reg(dev, DS3231_REG_STATUS, status);
}

esp_err_t ds3231_get_temperature_c(ds3231_dev_t *dev, float *temperature_c)
{
    if (!dev || !temperature_c) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[2];
    esp_err_t err = ds3231_read_regs(dev, DS3231_REG_TEMP_MSB, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    const int8_t whole = (int8_t)raw[0];
    const uint8_t quarter = (uint8_t)((raw[1] >> 6) & 0x03u);
    *temperature_c = (float)whole + ((float)quarter * 0.25f);
    return ESP_OK;
}
