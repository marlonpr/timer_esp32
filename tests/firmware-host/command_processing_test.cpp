#include "command_processor.h"
#include "factory_log.h"
#include "network_policy.h"
#include "protocol_codec.h"

#include <cstdint>
#include <iostream>
#include <string_view>

int g_info_log_calls = 0;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << "CHECK failed at line " << __LINE__                  \
                      << ": " #condition << '\n';                             \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

factory_timer::CommandPacket Parse(std::string_view text) {
    factory_timer::CommandPacket command{};
    factory_timer::ParseError error{};
    CHECK(factory_timer::ParseCommand(text, command, error));
    return command;
}

void CheckDebugMacroConfiguration() {
    int argument_evaluations = 0;
    FACTORY_DEBUG_LOGI("test", "value=%d", ++argument_evaluations);

#if CONFIG_FACTORY_DEBUG_LOGS
    CHECK(argument_evaluations == 1);
    CHECK(g_info_log_calls == 1);
#else
    CHECK(argument_evaluations == 0);
    CHECK(g_info_log_calls == 0);
#endif
}

void CheckLegacyCountdownBehavior() {
    constexpr int64_t start_received_us = 1000000;
    factory_timer::CountdownTimer timer(20);
    const auto start = Parse("FCT1|CMD|START|0000000000000001|3|500");

    const auto accepted = factory_timer::ProcessCommand(timer, start, start_received_us);
    CHECK(accepted.send_ack);
    CHECK(accepted.ack_result == factory_timer::AckResult::Accepted);
    CHECK(accepted.snapshot.state == factory_timer::TimerState::Armed);
    CHECK(timer.ScheduledStartMicroseconds() == 1500000);

    const auto duplicate = factory_timer::ProcessCommand(timer, start, start_received_us);
    CHECK(duplicate.ack_result == factory_timer::AckResult::Duplicate);
    CHECK(timer.ScheduledStartMicroseconds() == 1500000);
}

void CheckAbsoluteCountdownBehavior() {
    factory_timer::CountdownTimer timer(20);
    const auto start_at = Parse("FCT2|CMD|START_AT|0000000000000010|3|5000000");
    CHECK(start_at.type == factory_timer::CommandType::StartAt);
    CHECK(start_at.start_at_master_us == 5000000);

    // The firmware has already converted Master time to its own local timer domain.
    constexpr int64_t received_local_us = 1000000;
    constexpr int64_t local_target_us = 2000000;
    const auto accepted = factory_timer::ProcessCommand(
        timer, start_at, received_local_us, local_target_us);
    CHECK(accepted.ack_result == factory_timer::AckResult::Accepted);
    CHECK(timer.ScheduledStartMicroseconds() == local_target_us);

    // A duplicate arriving later must remain a duplicate, not move the target.
    const auto duplicate = factory_timer::ProcessCommand(
        timer, start_at, 1900000, local_target_us);
    CHECK(duplicate.ack_result == factory_timer::AckResult::Duplicate);
    CHECK(timer.ScheduledStartMicroseconds() == local_target_us);

    CHECK(timer.Update(1999999).state == factory_timer::TimerState::Armed);
    CHECK(timer.Update(2000000).state == factory_timer::TimerState::Running);

    factory_timer::CountdownTimer late_timer(20);
    const auto late = factory_timer::ProcessCommand(
        late_timer, start_at, 2100000, local_target_us);
    CHECK(late.ack_result == factory_timer::AckResult::Late);
    CHECK(late.snapshot.state == factory_timer::TimerState::Ready);
}

void CheckNetworkPolicy() {
    constexpr uint32_t kMask16 = 0xFFFF0000u;
    CHECK(factory_timer::IsAcceptedFactoryNetwork(0xC0A8057Bu, kMask16,
                                                   0xC0A80001u));
    CHECK(factory_timer::IsAcceptedFactoryNetwork(0xC0A8C807u, kMask16,
                                                   0xC0A80001u));
    CHECK(!factory_timer::IsAcceptedFactoryNetwork(0x0A2D00AFu, kMask16,
                                                    0x0A2D00F5u));
    CHECK(!factory_timer::IsAcceptedFactoryNetwork(0xC0A8057Bu, 0xFFFFFF00u,
                                                    0xC0A80001u));
    CHECK(!factory_timer::IsAcceptedFactoryNetwork(0xC0A8057Bu, kMask16,
                                                    0xC0A80101u));
}

void CheckProtocolFormats() {
    factory_timer::MasterPacket master{};
    factory_timer::ParseError error{};
    CHECK(factory_timer::ParseMasterPacket(
        "FCT2|SYNC|0123456789ABCDEF|123456", master, error));
    CHECK(master.type == factory_timer::MasterPacketType::SyncRequest);
    CHECK(master.sync_request.sync_id == 0x0123456789abcdefULL);
    CHECK(master.sync_request.master_t1_us == 123456);
    CHECK(master.sync_request.artificial_reply_delay_us == 0);

    CHECK(factory_timer::ParseMasterPacket(
        "FCT2|SYNC|0123456789ABCDEF|123456|250000", master, error));
    CHECK(master.type == factory_timer::MasterPacketType::SyncRequest);
    CHECK(master.sync_request.sync_id == 0x0123456789abcdefULL);
    CHECK(master.sync_request.master_t1_us == 123456);
    CHECK(master.sync_request.artificial_reply_delay_us == 250000);

    CHECK(!factory_timer::ParseMasterPacket(
        "FCT2|SYNC|0123456789ABCDEF|123456|1500001", master, error));
    CHECK(error == factory_timer::ParseError::Delay);

    CHECK(factory_timer::ParseMasterPacket(
        "FCT2|SYNC_SET|0123456789ABCDEF|-987654|4200", master, error));
    CHECK(master.type == factory_timer::MasterPacketType::SyncSet);
    CHECK(master.sync_set.master_minus_local_offset_us == -987654);
    CHECK(master.sync_set.best_rtt_us == 4200);

    char packet[factory_timer::kMaxPacketLength + 1]{};
    int length = factory_timer::FormatSyncReply(
        packet, sizeof(packet), "ESP02", 0x0123456789abcdefULL,
        100000, 40000, 40005);
    CHECK(length > 0);
    CHECK(std::string_view(packet, static_cast<std::size_t>(length)) ==
          "FCT2|SYNC_REPLY|ESP02|0123456789ABCDEF|100000|40000|40005");

    const auto start_at = Parse("FCT2|CMD|START_AT|0000000000000010|3|5000000");
    length = factory_timer::FormatAck(
        packet, sizeof(packet), "ESP01", start_at, factory_timer::AckResult::NotSynced);
    CHECK(std::string_view(packet, static_cast<std::size_t>(length)) ==
          "FCT1|ACK|ESP01|0000000000000010|START_AT|NOT_SYNCED");

    length = factory_timer::FormatStarted(
        packet, sizeof(packet), "ESP01", 0x10, 2000000, 5000002, 5000000);
    CHECK(std::string_view(packet, static_cast<std::size_t>(length)) ==
          "FCT2|STARTED|ESP01|0000000000000010|2000000|5000002|5000000");
}

} // namespace

int main() {
    CheckDebugMacroConfiguration();
    CheckLegacyCountdownBehavior();
    CheckAbsoluteCountdownBehavior();
    CheckNetworkPolicy();
    CheckProtocolFormats();
    if (failures != 0) return 1;
    std::cout << "factory timer host tests passed with CONFIG_FACTORY_DEBUG_LOGS="
              << CONFIG_FACTORY_DEBUG_LOGS << '\n';
    return 0;
}
