#pragma once

#include "countdown_timer.h"

#include <cstdint>

namespace factory_timer {

struct CommandProcessingResult {
    bool send_ack;
    AckResult ack_result;
    TimerSnapshot snapshot;
};

CommandProcessingResult ProcessCommand(CountdownTimer& timer, const CommandPacket& command,
                                       int64_t now_microseconds,
                                       int64_t absolute_local_start_microseconds = 0);

}  // namespace factory_timer
