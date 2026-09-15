#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool factory_display_backend_init(uint8_t brightness);
void factory_display_backend_clear(void);
void factory_display_backend_fill_rect(int x, int y, int width, int height,
                                       uint8_t r, uint8_t g, uint8_t b);
void factory_display_backend_flip(void);

#ifdef __cplusplus
}
#endif
