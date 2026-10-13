// Host test: a START_AT delayed past a confirmed RESET must not arm (firmware 6.24.0 chain fence).
// Build like segmented_program_test.cpp:
//   g++ -std=c++17 -Wall -Wextra -Werror -I firmware_v6_23_16/main late_start_after_reset_test.cpp firmware_v6_23_16/main/countdown_timer.cpp
#include "countdown_timer.h"
#include "segment_schedule.h"
#include <cassert>
#include <iostream>
using namespace factory_timer;
// The original segment START_AT (id 21, chained to id 20) is delayed in the network.
// The board only sees the controller's RESET (id 22), reports a safe state, and the
// controller confirms cancellation. The delayed original then arrives.
int main() {
    CountdownTimer t;
    CommandPacket prior{};prior.type=CommandType::Reset;prior.command_id=20;prior.duration_seconds=180;
    assert(t.Apply(prior,1)==AckResult::Accepted);
    CommandPacket original{};
    original.type=CommandType::StartAt;original.command_id=21;original.duration_seconds=180;
    original.start_at_master_us=9000000;original.plan_id=0x777;original.segment_index=2;
    original.repeat_count=3;original.first_repeat=4;original.total_repeats=10;
    original.expected_boot_id=0x456;original.expected_previous_command_id=20;
    CommandPacket reset{};reset.type=CommandType::Reset;reset.command_id=22;reset.duration_seconds=180;
    assert(t.Apply(reset,2)==AckResult::Accepted);             // only the RESET arrived
    assert(t.Apply(original,3,6000000)==AckResult::Invalid);   // late original is fenced
    auto retry=original;retry.command_id=23;retry.expected_previous_command_id=22;retry.start_at_master_us=12000000;
    assert(t.Apply(retry,4,9000000)==AckResult::Accepted);     // controller's retry (new id, new T*) arms
    assert(t.Apply(original,5,6000000)==AckResult::Busy);      // original still cannot arm on top of it
    std::cout<<"PASS: delayed original START_AT after a confirmed RESET is rejected; the retry arms\n";
}
