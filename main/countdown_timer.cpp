#include "countdown_timer.h"

#include <algorithm>

namespace factory_timer {

CountdownTimer::CountdownTimer(uint32_t default_duration_seconds)
    : duration_seconds_(std::clamp(default_duration_seconds, kMinDurationSeconds, kMaxDurationSeconds)),
      remaining_seconds_(duration_seconds_) {}

bool CountdownTimer::HasSeen(uint64_t command_id) const {
    return std::find(recent_command_ids_.begin(), recent_command_ids_.end(), command_id) !=
           recent_command_ids_.end();
}

void CountdownTimer::Remember(uint64_t command_id) {
    recent_command_ids_[next_recent_command_] = command_id;
    next_recent_command_ = (next_recent_command_ + 1) % recent_command_ids_.size();
}

AckResult CountdownTimer::Apply(const CommandPacket& command, int64_t now_microseconds,
                                int64_t absolute_local_start_microseconds) {
    if (command.type == CommandType::StatusRequest) return AckResult::Duplicate;
    if (HasSeen(command.command_id)) return AckResult::Duplicate;

    if (command.type == CommandType::StartAt && absolute_local_start_microseconds <= now_microseconds) {
        return AckResult::Late;
    }

    Remember(command.command_id);
    last_command_id_ = command.command_id;
    duration_seconds_ = command.duration_seconds;
    remaining_seconds_ = duration_seconds_;

    if (command.type == CommandType::Start) {
        scheduled_start_microseconds_ =
            now_microseconds + static_cast<int64_t>(command.start_delay_ms) * 1000;
        state_ = TimerState::Armed;
    } else if (command.type == CommandType::StartAt) {
        scheduled_start_microseconds_ = absolute_local_start_microseconds;
        state_ = TimerState::Armed;
    } else {
        scheduled_start_microseconds_ = 0;
        state_ = TimerState::Ready;
    }
    return AckResult::Accepted;
}

TimerSnapshot CountdownTimer::Update(int64_t now_microseconds) {
    if (state_ == TimerState::Armed && now_microseconds >= scheduled_start_microseconds_) {
        state_ = TimerState::Running;
    }

    if (state_ == TimerState::Running) {
        const int64_t elapsed = std::max<int64_t>(0, now_microseconds - scheduled_start_microseconds_);
        const uint64_t elapsed_seconds = static_cast<uint64_t>(elapsed / 1000000);
        if (elapsed_seconds >= duration_seconds_) {
            remaining_seconds_ = 0;
            state_ = TimerState::Finished;
        } else {
            remaining_seconds_ = duration_seconds_ - static_cast<uint32_t>(elapsed_seconds);
        }
    }
    return Snapshot();
}

TimerSnapshot CountdownTimer::Snapshot() const {
    return TimerSnapshot{state_, remaining_seconds_, duration_seconds_, last_command_id_};
}

}  // namespace factory_timer
