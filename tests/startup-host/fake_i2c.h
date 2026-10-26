#pragma once
#include "driver/i2c_master.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    esp_err_t create_result, probe_result, add_result, read_result, write_result;
    unsigned creates, probes, adds, deletes, reads, writes;
    unsigned simulated_wait_ms;
    bool bus_live, device_live;
    uint8_t registers[256];
} fake_i2c_state_t;
extern fake_i2c_state_t fake_i2c;
void fake_i2c_reset(void);
#ifdef __cplusplus
}
#endif
