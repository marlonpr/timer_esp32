#include "host_idf.h"
struct host_gpio_dev_t {
    uint32_t out_w1ts{}, out_w1tc{};
    struct {uint32_t val{};} out1_w1ts, out1_w1tc;
};
inline host_gpio_dev_t GPIO{};
