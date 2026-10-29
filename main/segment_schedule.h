#pragma once

#include <algorithm>
#include <cstdint>

namespace factory_timer {

constexpr uint32_t kMaximumRepeatDurationSeconds = 5999;
constexpr uint32_t kMaximumSegmentDurationSeconds = 6000;

constexpr uint32_t kRepetitionLogoSeconds = 4;

inline uint64_t SegmentSeconds(uint32_t duration, uint32_t repeats, uint32_t logo_seconds = 0,
                               uint32_t repeat_start_seconds = 0) {
    return (static_cast<uint64_t>(duration) + logo_seconds) * repeats +
           static_cast<uint64_t>(repeat_start_seconds) * (repeats == 0 ? 0 : repeats - 1);
}

inline bool ValidSegment(uint32_t duration, uint32_t repeats, uint32_t first,
                         uint32_t total, uint32_t segment, uint32_t logo_seconds = 0,
                         uint32_t repeat_start_seconds = 0) {
    // The last logo follows the last countdown; it never precedes another
    // countdown at an older sync age. Internal logo gaps do count against cap.
    return duration > 0 && duration <= kMaximumRepeatDurationSeconds && repeats > 0 &&
           first > 0 && total > 0 && segment > 0 &&
           (logo_seconds == 0 || logo_seconds == kRepetitionLogoSeconds) &&
           (repeat_start_seconds == 0 || (repeat_start_seconds == kRepetitionLogoSeconds && logo_seconds == kRepetitionLogoSeconds)) &&
           static_cast<uint64_t>(first) + repeats - 1 <= total &&
           SegmentSeconds(duration, repeats, logo_seconds, repeat_start_seconds) - logo_seconds <= kMaximumSegmentDurationSeconds;
}

inline bool IsRepeatStart(uint32_t duration, uint32_t repeats, uint64_t elapsed,
                          uint32_t logo_seconds = 0, uint32_t repeat_start_seconds = 0) {
    return duration > 0 && repeat_start_seconds > 0 &&
           elapsed < SegmentSeconds(duration, repeats, logo_seconds, repeat_start_seconds) &&
           elapsed % (static_cast<uint64_t>(duration) + logo_seconds + repeat_start_seconds) >= duration + logo_seconds;
}

inline uint32_t RepeatRemaining(uint32_t duration, uint32_t repeats, uint64_t elapsed,
                                uint32_t logo_seconds = 0, uint32_t repeat_start_seconds = 0) {
    if (duration == 0 || elapsed >= SegmentSeconds(duration, repeats, logo_seconds, repeat_start_seconds)) return 0;
    const uint32_t phase = static_cast<uint32_t>(elapsed % (static_cast<uint64_t>(duration) + logo_seconds + repeat_start_seconds));
    return phase < duration ? duration - phase : 0;
}

inline uint32_t RepeatIndex(uint32_t duration, uint32_t repeats, uint32_t first,
                            uint64_t elapsed, uint32_t logo_seconds = 0, uint32_t repeat_start_seconds = 0) {
    if (duration == 0 || repeats == 0) return first;
    const uint64_t index = elapsed / (static_cast<uint64_t>(duration) + logo_seconds + repeat_start_seconds) +
        (IsRepeatStart(duration, repeats, elapsed, logo_seconds, repeat_start_seconds) ? 1 : 0);
    return first + static_cast<uint32_t>(std::min<uint64_t>(index, repeats - 1));
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
