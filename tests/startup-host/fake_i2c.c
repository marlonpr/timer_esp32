#include "fake_i2c.h"
#include "ds3231.h"
#include <assert.h>
#include <string.h>

fake_i2c_state_t fake_i2c;
static int bus_token, device_token;
void fake_i2c_reset(void) { memset(&fake_i2c, 0, sizeof(fake_i2c)); }

static esp_err_t transfer_result(esp_err_t result, int timeout_ms)
{
    /* Reject the old unlimited timeout as well as accidentally unbounded calls. */
    assert(timeout_ms > 0 && timeout_ms <= 100);
    if (result == ESP_ERR_TIMEOUT) fake_i2c.simulated_wait_ms += (unsigned)timeout_ms;
    return result;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *out)
{
    ++fake_i2c.creates;
    assert(cfg->i2c_port == I2C_NUM_0 && cfg->sda_io_num == 21 && cfg->scl_io_num == 22);
    assert(!cfg->flags.enable_internal_pullup);
    if (fake_i2c.create_result != ESP_OK) return fake_i2c.create_result;
    assert(!fake_i2c.bus_live);
    fake_i2c.bus_live = true;
    *out = &bus_token;
    return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms)
{
    assert(bus == &bus_token && fake_i2c.bus_live && !fake_i2c.device_live);
    assert(address == 0x68);
    ++fake_i2c.probes;
    return transfer_result(fake_i2c.probe_result, timeout_ms);
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg, i2c_master_dev_handle_t *out)
{
    assert(bus == &bus_token && fake_i2c.bus_live);
    assert(fake_i2c.probes == 1 && fake_i2c.probe_result == ESP_OK);
    assert(cfg->dev_addr_length == I2C_ADDR_BIT_LEN_7 && cfg->device_address == 0x68);
    assert(cfg->scl_speed_hz == 100000);
    ++fake_i2c.adds;
    if (fake_i2c.add_result != ESP_OK) return fake_i2c.add_result;
    fake_i2c.device_live = true;
    *out = &device_token;
    return ESP_OK;
}

esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus)
{
    assert(bus == &bus_token && fake_i2c.bus_live && !fake_i2c.device_live);
    ++fake_i2c.deletes;
    fake_i2c.bus_live = false;
    return ESP_OK;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len, int timeout_ms)
{
    assert(dev == &device_token && fake_i2c.device_live && len >= 2);
    assert((size_t)data[0] + len - 1 <= sizeof(fake_i2c.registers));
    ++fake_i2c.writes;
    esp_err_t err = transfer_result(fake_i2c.write_result, timeout_ms);
    if (err == ESP_OK) memcpy(fake_i2c.registers + data[0], data + 1, len - 1);
    return err;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *reg, size_t reg_len,
                                      uint8_t *out, size_t len, int timeout_ms)
{
    assert(dev == &device_token && fake_i2c.device_live && reg_len == 1);
    assert((size_t)*reg + len <= sizeof(fake_i2c.registers));
    ++fake_i2c.reads;
    esp_err_t err = transfer_result(fake_i2c.read_result, timeout_ms);
    if (err == ESP_OK) memcpy(out, fake_i2c.registers + *reg, len);
    return err;
}
