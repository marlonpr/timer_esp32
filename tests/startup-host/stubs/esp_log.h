#pragma once
#include <stdarg.h>
static inline void startup_host_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
#define ESP_LOGI(...) startup_host_log(__VA_ARGS__)
#define ESP_LOGW(...) startup_host_log(__VA_ARGS__)
#define ESP_LOGE(...) startup_host_log(__VA_ARGS__)
