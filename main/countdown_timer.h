#pragma once

#include "protocol_codec.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace factory_timer {

struct TimerSnapshot {
    TimerState state;
    uint32_t remaining_seconds;
    uint32_t duration_seconds;
    uint64_t last_command_id;

    bool operator==(const TimerSnapshot& other) const {
        return state == other.state && remaining_seconds == other.remaining_seconds &&
               duration_seconds == other.duration_seconds && last_command_id == other.last_command_id;
    }
    bool operator!=(const TimerSnapshot& other) const { return !(*this == other); }
};

class CountdownTimer {
public:
    explicit CountdownTimer(uint32_t default_duration_seconds = 20);

    AckResult Apply(const CommandPacket& command, int64_t now_microseconds,
                    int64_t absolute_local_start_microseconds = 0);
    TimerSnapshot Update(int64_t now_microseconds);
    TimerSnapshot Snapshot() const;
    int64_t ScheduledStartMicroseconds() const { return scheduled_start_microseconds_; }

private:
    bool HasSeen(uint64_t command_id) const;
    void Remember(uint64_t command_id);

    static constexpr std::size_t kRecentCommandCount = 16;
    std::array<uint64_t, kRecentCommandCount> recent_command_ids_{};
    std::size_t next_recent_command_{};
    TimerState state_{TimerState::Ready};
    uint32_t duration_seconds_;
    uint32_t remaining_seconds_;
    uint64_t last_command_id_{};
    int64_t scheduled_start_microseconds_{};
};

}  // namespace factory_timer
