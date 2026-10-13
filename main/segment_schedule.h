#pragma once

#include <algorithm>
#include <cstdint>

namespace factory_timer {

constexpr uint32_t kMaximumRepeatDurationSeconds = 5999;
constexpr uint32_t kMaximumSegmentDurationSeconds = 6000;

inline uint64_t SegmentSeconds(uint32_t duration, uint32_t repeats) {
    return static_cast<uint64_t>(duration) * repeats;
}

inline bool ValidSegment(uint32_t duration, uint32_t repeats, uint32_t first,
                         uint32_t total, uint32_t segment) {
    return duration > 0 && duration <= kMaximumRepeatDurationSeconds && repeats > 0 &&
           first > 0 && total > 0 && segment > 0 &&
           static_cast<uint64_t>(first) + repeats - 1 <= total &&
           SegmentSeconds(duration, repeats) <= kMaximumSegmentDurationSeconds;
}

inline uint32_t RepeatRemaining(uint32_t duration, uint32_t repeats, uint64_t elapsed) {
    if (duration == 0 || elapsed >= SegmentSeconds(duration, repeats)) return 0;
    // At an internal boundary, go directly from 00:01 to the next repeat's D.
    return duration - static_cast<uint32_t>(elapsed % duration);
}

inline uint32_t RepeatIndex(uint32_t duration, uint32_t repeats, uint32_t first,
                            uint64_t elapsed) {
    if (duration == 0 || repeats == 0) return first;
    return first + static_cast<uint32_t>(std::min<uint64_t>(elapsed / duration, repeats - 1));
}

// A complete fit window after a discontinuity is required; old cumulative
// fault counts do not permanently exclude a board that has recovered.
class RtcRecoveryWindow {
public:
    uint16_t Observe(uint64_t accepted, uint64_t missing, uint64_t holdovers,
                     uint32_t drops, bool locked) {
        if (!initialized_) {
            initialized_ = true;
            recovery_base_ = locked && missing == 0 && holdovers == 0 && drops == 0 ? 0 : accepted;
        } else if (accepted < accepted_ || missing != missing_ ||
                   holdovers != holdovers_ || drops != drops_ || (!locked && locked_)) {
            recovery_base_ = accepted;
        }
        locked_ = locked;
        accepted_ = accepted;
        missing_ = missing;
        holdovers_ = holdovers;
        drops_ = drops;
        return static_cast<uint16_t>(std::min<uint64_t>(
            accepted >= recovery_base_ ? accepted - recovery_base_ : 0, 129));
    }

private:
    bool initialized_{}, locked_{};
    uint64_t accepted_{}, missing_{}, holdovers_{}, recovery_base_{};
    uint32_t drops_{};
};

} // namespace factory_timer
