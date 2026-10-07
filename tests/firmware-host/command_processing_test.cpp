#include "command_processor.h"
#include "factory_log.h"
#include "network_policy.h"
#include "protocol_codec.h"

#include <algorithm>
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

void CheckDisciplinedRunningClock() {
    factory_timer::CountdownTimer timer(20);
    const auto start_at = Parse("FCT2|CMD|START_AT|0000000000000020|3|2000000");

    const auto accepted = factory_timer::ProcessCommand(
        timer, start_at, 1000000, 2000000);
    CHECK(accepted.ack_result == factory_timer::AckResult::Accepted);

    // START is still decided in raw local time, but the running epoch is taken
    // from the independent disciplined clock domain.
    auto snapshot = timer.Update(2000000, 10000000);
    CHECK(snapshot.state == factory_timer::TimerState::Running);
    CHECK(snapshot.remaining_seconds == 3);
    CHECK(timer.StartRunningMicroseconds() == 10000000);
    CHECK(timer.EndRunningMicroseconds() == 13000000);

    // Raw local time can advance by more than a second without decrementing if
    // the disciplined domain has not yet crossed its absolute 1-second boundary.
    snapshot = timer.Update(3100000, 10999999);
    CHECK(snapshot.state == factory_timer::TimerState::Running);
    CHECK(snapshot.remaining_seconds == 3);

    snapshot = timer.Update(3100001, 11000000);
    CHECK(snapshot.remaining_seconds == 2);

    snapshot = timer.Update(5200000, 13000000);
    CHECK(snapshot.state == factory_timer::TimerState::Finished);
    CHECK(snapshot.remaining_seconds == 0);
}

void CheckNetworkPolicy() {
    constexpr uint32_t kMask24 = 0xFFFFFF00u;
    CHECK(factory_timer::IsAcceptedFactoryNetwork(0xC0A8007Bu, kMask24,
                                                   0xC0A80001u));
    CHECK(factory_timer::IsAcceptedFactoryNetwork(0xC0A800F0u, kMask24,
                                                   0xC0A80001u));
    CHECK(!factory_timer::IsAcceptedFactoryNetwork(0xC0A80107u, kMask24,
                                                    0xC0A80001u));
    CHECK(!factory_timer::IsAcceptedFactoryNetwork(0x0A2D00AFu, kMask24,
                                                    0x0A2D00F5u));
    CHECK(!factory_timer::IsAcceptedFactoryNetwork(0xC0A8007Bu, 0xFFFF0000u,
                                                    0xC0A80001u));
    CHECK(!factory_timer::IsAcceptedFactoryNetwork(0xC0A8007Bu, kMask24,
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
    CHECK(!master.sync_request.request_die_temperature);

    CHECK(factory_timer::ParseMasterPacket(
        "FCT2|SYNC|0123456789ABCDEF|123456|250000", master, error));
    CHECK(master.type == factory_timer::MasterPacketType::SyncRequest);
    CHECK(master.sync_request.sync_id == 0x0123456789abcdefULL);
    CHECK(master.sync_request.master_t1_us == 123456);
    CHECK(master.sync_request.artificial_reply_delay_us == 250000);
    CHECK(!master.sync_request.request_die_temperature);

    CHECK(factory_timer::ParseMasterPacket(
        "FCT2|SYNC|0123456789ABCDEF|123456|0|TEMP", master, error));
    CHECK(master.type == factory_timer::MasterPacketType::SyncRequest);
    CHECK(master.sync_request.sync_id == 0x0123456789abcdefULL);
    CHECK(master.sync_request.master_t1_us == 123456);
    CHECK(master.sync_request.artificial_reply_delay_us == 0);
    CHECK(master.sync_request.request_die_temperature);

    CHECK(!factory_timer::ParseMasterPacket(
        "FCT2|SYNC|0123456789ABCDEF|123456|1500001", master, error));
    CHECK(error == factory_timer::ParseError::Delay);

    CHECK(factory_timer::ParseMasterPacket(
        "FCT2|SYNC_SET|0123456789ABCDEF|-987654|4200", master, error));
    CHECK(master.type == factory_timer::MasterPacketType::SyncSet);
    CHECK(master.sync_set.master_minus_local_offset_us == -987654);
    CHECK(master.sync_set.best_rtt_us == 4200);
    CHECK(master.sync_set.offset_epoch_local_us == 0);

    CHECK(factory_timer::ParseMasterPacket(
        "FCT2|SYNC_SET|0123456789ABCDEF|-987654|4200|7654321", master, error));
    CHECK(master.type == factory_timer::MasterPacketType::SyncSet);
    CHECK(master.sync_set.master_minus_local_offset_us == -987654);
    CHECK(master.sync_set.best_rtt_us == 4200);
    CHECK(master.sync_set.offset_epoch_local_us == 7654321);

    char packet[factory_timer::kMaxPacketLength + 1]{};
    int length = factory_timer::FormatStatus(
        packet, sizeof(packet), "ESP03", 0x0123456789abcdefULL,
        factory_timer::TimerState::Running, 19, -57, 6, "AA:BB:CC:DD:EE:FF", "LOCKED",
        129, 0.785, 0, true, -6.500001, 2, 25.25, 1, 0x07,
        -987654, 7654321, 7654000, -987333, 0, 0, 28, 49, 0,
        12345, 0, 0);
    CHECK(length > 0);
    CHECK(std::string_view(packet, static_cast<std::size_t>(length)) ==
          "FCT2|STATUS|ESP03|0123456789ABCDEF|RUNNING|19|-57|6|AA:BB:CC:DD:EE:FF|LOCKED|129|0.785|0|1|-6.500001|2|25.25|1|7|-987654|7654321|7654000|-987333|0|0|28|49|0|3039|0|0|0|0|0|0|NONE|0|0|0|0|0|0|0|00000000");

    // The extended fleet-health STATUS must still fit inside the advertised
    // protocol envelope even at conservative numeric extremes.
    length = factory_timer::FormatStatus(
        packet, sizeof(packet), "ESP1234567890123", UINT64_MAX,
        factory_timer::TimerState::Finished, factory_timer::kMaxDurationSeconds,
        -127, 255, "FF:FF:FF:FF:FF:FF", "UNINITIALIZED", UINT16_MAX,
        1000000.0, UINT32_MAX, true, -1000000.0, UINT64_MAX, -1000.0, -1, 0x07,
        INT64_MIN, INT64_MAX, INT64_MAX, INT64_MIN, INT64_MIN, INT64_MIN,
        INT64_MIN, INT64_MIN, UINT32_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
        true, UINT32_MAX, UINT32_MAX, UINT32_MAX, "TASKNAME1234567",
        UINT32_MAX, UINT32_MAX, true, UINT32_MAX, UINT32_MAX, true,
        UINT32_MAX, "deadbeef");
    CHECK(length > 0);
    CHECK(static_cast<std::size_t>(length) <= factory_timer::kMaxPacketLength);
    std::cout << "STATUS_MAX_WIDTH_BYTES=" << length
              << " STATUS_PLUS_SIX_HEX_COUNTERS_BYTES=" << length + 6 * 9 << '\n';
    // The new counters/windows are deliberately separate from STATUS.
    factory_timer::RunDiagnosticFields diagnostic{};
    diagnostic.valid = true;
    diagnostic.period_us = UINT32_MAX;
    diagnostic.threshold_us = UINT32_MAX;
    diagnostic.monitor_start_us = INT64_MIN;
    diagnostic.tstar_local_us = INT64_MIN;
    diagnostic.monitor_end_us = INT64_MIN;
    diagnostic.first_alarm_offset_us = UINT32_MAX;
    diagnostic.expected_periods = UINT64_MAX;
    diagnostic.sample_callbacks = UINT32_MAX;
    diagnostic.missed_periods = UINT32_MAX;
    diagnostic.cpu0_events_ge_50us = UINT32_MAX;
    diagnostic.rtc_discipline_events_ge_50us = UINT32_MAX;
    diagnostic.wifi_events_ge_50us = UINT32_MAX;
    diagnostic.udp_events_ge_50us = UINT32_MAX;
    diagnostic.commit_late_events = UINT32_MAX;
    diagnostic.commit_ge_300us = UINT32_MAX;
    diagnostic.rearm_failures = UINT32_MAX;
    length = factory_timer::FormatRunDiagnostic(packet, sizeof(packet),
        "ESP1234567890123", UINT64_MAX, diagnostic);
    CHECK(length > 0 && static_cast<std::size_t>(length) <= factory_timer::kMaxPacketLength);
    std::cout << "RUN_DIAG_MAX_WIDTH_BYTES=" << length << '\n';
    CHECK(std::count(packet, packet + length, '|') == 20);

    length = factory_timer::FormatSyncReply(
        packet, sizeof(packet), "ESP02", 0x0123456789abcdefULL,
        100000, 40000, 40005, 39950);
    CHECK(length > 0);
    CHECK(std::string_view(packet, static_cast<std::size_t>(length)) ==
          "FCT2|SYNC_REPLY|ESP02|0123456789ABCDEF|100000|40000|40005|0|39950");

    length = factory_timer::FormatSyncReply(
        packet, sizeof(packet), "ESP03", 0x0123456789abcdefULL,
        100000, 40000, 40005, 39950, 0, true, 41250);
    CHECK(length > 0);
    CHECK(std::string_view(packet, static_cast<std::size_t>(length)) ==
          "FCT2|SYNC_REPLY|ESP03|0123456789ABCDEF|100000|40000|40005|0|39950|41250");

    const auto start_at = Parse("FCT2|CMD|START_AT|0000000000000010|3|5000000");
    length = factory_timer::FormatAck(
        packet, sizeof(packet), "ESP01", start_at, factory_timer::AckResult::NotSynced);
    CHECK(std::string_view(packet, static_cast<std::size_t>(length)) ==
          "FCT1|ACK|ESP01|0000000000000010|START_AT|NOT_SYNCED");

    length = factory_timer::FormatSyncApplied(
        packet, sizeof(packet), "ESP01", 0x0123456789abcdefULL,
        -987654, 4200, 7654321);
    CHECK(length > 0);
    CHECK(std::string_view(packet, static_cast<std::size_t>(length)) ==
          "FCT2|SYNC_APPLIED|ESP01|0123456789ABCDEF|-987654|4200|7654321");

    length = factory_timer::FormatStarted(
        packet, sizeof(packet), "ESP01", 0x10, 2000000, 5000002, 5000000);
    CHECK(std::string_view(packet, static_cast<std::size_t>(length)) ==
          "FCT2|STARTED|ESP01|0000000000000010|2000000|5000002|5000000");
}

} // namespace


void TestBrightnessCommandParsing() {
    const auto brightness = Parse("FCT2|CMD|BRIGHTNESS|00000000000000B1|37|0");
    CHECK(brightness.type == factory_timer::CommandType::Brightness);
    CHECK(brightness.command_id == 0xB1);
    CHECK(brightness.brightness_percent == 37);

    factory_timer::CommandPacket ignored{};
    factory_timer::ParseError error{};
    CHECK(!factory_timer::ParseCommand(
        "FCT2|CMD|BRIGHTNESS|00000000000000B1|101|0", ignored, error));
    CHECK(error == factory_timer::ParseError::Brightness);
}

int main() {
    TestBrightnessCommandParsing();
    CheckDebugMacroConfiguration();
    CheckLegacyCountdownBehavior();
    CheckAbsoluteCountdownBehavior();
    CheckDisciplinedRunningClock();
    CheckNetworkPolicy();
    CheckProtocolFormats();
    if (failures != 0) return 1;
    std::cout << "factory timer host tests passed with CONFIG_FACTORY_DEBUG_LOGS="
              << CONFIG_FACTORY_DEBUG_LOGS << '\n';
    return 0;
}
