#include "countdown_timer.h"
#include "segment_schedule.h"

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
    if (HasSeen(command.command_id)) {
        if (command.plan_id != 0) {
            for (const auto& old : recent_commands_) {
                if (old.command_id == command.command_id &&
                    (old.plan_id != command.plan_id || old.segment_index != command.segment_index ||
                     old.duration_seconds != command.duration_seconds || old.repeat_count != command.repeat_count ||
                     old.first_repeat != command.first_repeat || old.total_repeats != command.total_repeats ||
                     old.start_at_master_us != command.start_at_master_us ||
                     old.expected_boot_id != command.expected_boot_id ||
                     old.expected_previous_command_id != command.expected_previous_command_id)) return AckResult::Invalid;
            }
        }
        return AckResult::Duplicate;
    }
    if (segment_.plan_id != 0 && (state_ == TimerState::Armed || state_ == TimerState::Running) &&
        (command.type == CommandType::Start || command.type == CommandType::StartAt)) return AckResult::Busy;
    if (command.plan_id != 0) {
        if (command.expected_boot_id == 0 || command.expected_previous_command_id != last_command_id_)
            return AckResult::Invalid;
        if (command.type != CommandType::StartAt ||
            !ValidSegment(command.duration_seconds, command.repeat_count, command.first_repeat,
                          command.total_repeats, command.segment_index)) return AckResult::Invalid;
        if (state_ == TimerState::Armed || state_ == TimerState::Running) return AckResult::Busy;
        if (command.plan_id == last_plan_id_ && (command.segment_index < last_segment_index_ ||
            (command.segment_index == last_segment_index_ && !cancelled_before_start_))) return AckResult::Invalid;
    }

    if (command.type == CommandType::StartAt && absolute_local_start_microseconds <= now_microseconds) {
        return AckResult::Late;
    }

    if (command.type == CommandType::Reset && state_ == TimerState::Armed)
        cancelled_before_start_ = true;
    recent_commands_[next_recent_command_] = command;
    Remember(command.command_id);
    last_command_id_ = command.command_id;
    duration_seconds_ = command.duration_seconds;
    remaining_seconds_ = duration_seconds_;
    start_running_microseconds_ = 0;
    end_running_microseconds_ = 0;
    scheduled_running_microseconds_ = 0;
    elapsed_segment_seconds_ = 0;
    elapsed_segment_microseconds_ = 0;
    segment_ = command;
    if (command.plan_id != 0) {
        cancelled_before_start_ = false;
        last_plan_id_ = command.plan_id;
        last_segment_index_ = command.segment_index;
    }

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
    return Update(now_microseconds, now_microseconds);
}

TimerSnapshot CountdownTimer::Update(int64_t now_local_microseconds,
                                     int64_t now_running_microseconds) {
    if (state_ == TimerState::Armed &&
        now_local_microseconds >= scheduled_start_microseconds_) {
        state_ = TimerState::Running;
        start_running_microseconds_ = scheduled_running_microseconds_ > 0
            ? scheduled_running_microseconds_ : now_running_microseconds;
        end_running_microseconds_ =
            start_running_microseconds_ +
            static_cast<int64_t>(SegmentSeconds(duration_seconds_, segment_.repeat_count)) * 1000000LL;
    }

    if (state_ == TimerState::Running) {
        const int64_t elapsed =
            std::max<int64_t>(0, now_running_microseconds - start_running_microseconds_);
        elapsed_segment_microseconds_ = std::min<int64_t>(elapsed,
            static_cast<int64_t>(SegmentSeconds(duration_seconds_, segment_.repeat_count)) * 1000000LL);
        const uint64_t elapsed_seconds = static_cast<uint64_t>(elapsed / 1000000LL);
        elapsed_segment_seconds_ = static_cast<uint32_t>(std::min<uint64_t>(
            elapsed_seconds, SegmentSeconds(duration_seconds_, segment_.repeat_count)));
        if (elapsed_seconds >= SegmentSeconds(duration_seconds_, segment_.repeat_count)) {
            remaining_seconds_ = 0;
            state_ = TimerState::Finished;
        } else {
            remaining_seconds_ =
                RepeatRemaining(duration_seconds_, segment_.repeat_count, elapsed_seconds);
        }
    }
    return Snapshot();
}

TimerSnapshot CountdownTimer::Snapshot() const {
    return TimerSnapshot{state_, remaining_seconds_, duration_seconds_, last_command_id_,
        segment_.plan_id, segment_.segment_index, segment_.repeat_count, segment_.first_repeat,
        segment_.total_repeats, RepeatIndex(duration_seconds_, segment_.repeat_count,
            segment_.first_repeat, elapsed_segment_seconds_), elapsed_segment_seconds_, segment_.start_at_master_us, elapsed_segment_microseconds_};
}

}  // namespace factory_timer
