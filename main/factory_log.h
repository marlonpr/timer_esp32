#pragma once

#include "esp_log.h"
#include "sdkconfig.h"

#if defined(CONFIG_FACTORY_DEBUG_LOGS) && CONFIG_FACTORY_DEBUG_LOGS
#define FACTORY_DEBUG_LOGI(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#else
#define FACTORY_DEBUG_LOGI(...)                                                \
	do {                                                                       \
	} while (0)
#endif
