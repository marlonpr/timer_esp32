#pragma once

#include <utility>

extern int g_info_log_calls;

template <typename... Args>
void FakeEspLogI(const char *, const char *, Args &&...) {
	++g_info_log_calls;
}

#define ESP_LOGI(...) FakeEspLogI(__VA_ARGS__)
