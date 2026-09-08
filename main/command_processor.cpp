#include "command_processor.h"

namespace factory_timer {

CommandProcessingResult ProcessCommand(CountdownTimer& timer, const CommandPacket& command,
                                       int64_t now_microseconds,
                                       int64_t absolute_local_start_microseconds) {
    if (command.type == CommandType::StatusRequest) {
        return CommandProcessingResult{false, AckResult::Accepted, timer.Snapshot()};
    }

    const auto ack_result = timer.Apply(command, now_microseconds, absolute_local_start_microseconds);
    auto snapshot = timer.Snapshot();
    // Armed -> Running is intentionally owned by the high-resolution esp_timer
    // scheduler in firmware. Command processing may refresh an already-running
    // countdown, but must never start an armed countdown from UDP task context.
    if (snapshot.state != TimerState::Armed) {
        snapshot = timer.Update(now_microseconds);
    }
    return CommandProcessingResult{true, ack_result, snapshot};
}

}  // namespace factory_timer
