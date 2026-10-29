#pragma once
#include <cstdint>

namespace factory_output {
// GPIO33 is active-LOW: LOW while signaling, HIGH while inactive/cancelled.
bool Init();
bool Ready();
void Schedule(int64_t start_disciplined_us, uint32_t duration_seconds,
              uint32_t repeats, uint32_t logo_seconds, uint32_t repeat_start_seconds = 0);
void Cancel();
}
