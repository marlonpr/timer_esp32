#include "ds3231.h"
#include "fake_i2c.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    ds3231_dev_t rtc = {0};
    assert(init_ds3231(NULL) == ESP_ERR_INVALID_ARG);
    for (unsigned fault = 0; fault < 4; ++fault) {
        fake_i2c_reset();
        esp_err_t expected = ESP_ERR_TIMEOUT;
        if (fault == 0) fake_i2c.create_result = expected = ESP_ERR_NO_MEM;
        if (fault == 1) fake_i2c.probe_result = expected = ESP_ERR_NOT_FOUND;
        if (fault == 2) fake_i2c.probe_result = ESP_ERR_TIMEOUT;
        if (fault == 3) fake_i2c.add_result = expected = ESP_ERR_NO_MEM;
        assert(init_ds3231(&rtc) == expected);
        assert(!rtc.bus && !rtc.dev && !fake_i2c.bus_live && !fake_i2c.device_live);
        assert(fake_i2c.deletes == (fault == 0 ? 0u : 1u));
        assert(fake_i2c.adds == (fault == 3 ? 1u : 0u));
        assert(fake_i2c.reads == 0 && fake_i2c.writes == 0);
        assert(fake_i2c.simulated_wait_ms <= 100);
    }
    fake_i2c_reset();
    assert(init_ds3231(&rtc) == ESP_OK);
    assert(rtc.bus && rtc.dev && fake_i2c.probes == 1 && fake_i2c.adds == 1);

    const ds3231_time_t time = {2026, 10, 19, 17, 42, 59, 1};
    ds3231_time_t received = {0};
    assert(ds3231_set_time(&rtc, &time) == ESP_OK);
    assert(fake_i2c.registers[0] == 0x59 && fake_i2c.registers[2] == 0x17);
    assert(ds3231_get_time(&rtc, &received) == ESP_OK);
    assert(received.year == time.year && received.month == time.month && received.day == time.day);
    assert(received.hour == time.hour && received.minute == time.minute && received.second == time.second);
    assert(received.day_of_week == time.day_of_week);

    fake_i2c.registers[0x0e] = 0xff;
    assert(ds3231_configure_1hz_sqw(&rtc) == ESP_OK);
    assert(fake_i2c.registers[0x0e] == 0x20);
    fake_i2c.registers[0x0f] = 0x8b;
    bool osf = false;
    assert(ds3231_get_oscillator_stop_flag(&rtc, &osf) == ESP_OK && osf);
    assert(ds3231_clear_oscillator_stop_flag(&rtc) == ESP_OK);
    assert(fake_i2c.registers[0x0f] == 0x0b);

    float temperature = 0;
    fake_i2c.registers[0x11] = 26; fake_i2c.registers[0x12] = 0x40;
    assert(ds3231_get_temperature_c(&rtc, &temperature) == ESP_OK && temperature == 26.25f);
    fake_i2c.registers[0x11] = 0xff; fake_i2c.registers[0x12] = 0xc0;
    assert(ds3231_get_temperature_c(&rtc, &temperature) == ESP_OK && temperature == -0.25f);

    fake_i2c.read_result = ESP_ERR_TIMEOUT;
    temperature = 123;
    received = time;
    osf = true;
    unsigned writes = fake_i2c.writes;
    assert(ds3231_get_temperature_c(&rtc, &temperature) == ESP_ERR_TIMEOUT && temperature == 123);
    assert(ds3231_get_time(&rtc, &received) == ESP_ERR_TIMEOUT && received.year == 2026);
    assert(ds3231_get_oscillator_stop_flag(&rtc, &osf) == ESP_ERR_TIMEOUT && osf);
    assert(ds3231_configure_1hz_sqw(&rtc) == ESP_ERR_TIMEOUT && fake_i2c.writes == writes);
    assert(ds3231_clear_oscillator_stop_flag(&rtc) == ESP_ERR_TIMEOUT && fake_i2c.writes == writes);

    fake_i2c.read_result = ESP_OK;
    fake_i2c.write_result = ESP_ERR_TIMEOUT;
    assert(ds3231_set_time(&rtc, &time) == ESP_ERR_TIMEOUT);
    assert(ds3231_configure_1hz_sqw(&rtc) == ESP_ERR_TIMEOUT);
    assert(ds3231_clear_oscillator_stop_flag(&rtc) == ESP_ERR_TIMEOUT);
    ds3231_dev_t invalid = {0};
    assert(ds3231_get_temperature_c(&invalid, &temperature) == ESP_ERR_INVALID_ARG);
    assert(ds3231_set_time(&invalid, &time) == ESP_ERR_INVALID_ARG);
    puts("DS3231_DRIVER=PASS probe/NACK/bus timeout/allocation cleanup/calendar/SQW/OSF/temperature/read-write timeouts");
    return 0;
}
