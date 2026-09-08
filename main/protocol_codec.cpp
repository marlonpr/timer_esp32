#include "protocol_codec.h"

#include <array>
#include <charconv>
#include <cstdio>

namespace factory_timer {
namespace {

constexpr std::string_view kVersion1 = "FCT1";
constexpr std::string_view kVersion2 = "FCT2";

template <std::size_t N>
bool SplitExact(std::string_view input, std::array<std::string_view, N>& fields) {
    std::size_t begin = 0;
    for (std::size_t i = 0; i < N; ++i) {
        const auto separator = input.find('|', begin);
        if (i + 1 == N) {
            if (separator != std::string_view::npos) return false;
            fields[i] = input.substr(begin);
            return true;
        }
        if (separator == std::string_view::npos) return false;
        fields[i] = input.substr(begin, separator - begin);
        begin = separator + 1;
    }
    return false;
}

template <typename T>
bool ParseDecimal(std::string_view value, T& output) {
    if (value.empty()) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output, 10);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

bool ParseCommandId(std::string_view value, uint64_t& output) {
    if (value.size() != 16) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output, 16);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() && output != 0;
}

bool ValidDeviceId(std::string_view value) {
    if (value.empty() || value.size() > 16) return false;
    for (const char character : value) {
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

bool ValidateEnvelope(std::string_view packet, ParseError& error) {
    error = ParseError::None;
    if (packet.empty()) {
        error = ParseError::Empty;
        return false;
    }
    if (packet.size() > kMaxPacketLength) {
        error = ParseError::TooLong;
        return false;
    }
    for (const unsigned char character : packet) {
        if (character < 0x20 || character > 0x7e) {
            error = ParseError::NonPrintable;
            return false;
        }
    }
    return true;
}

bool ParseCommandFields(std::string_view packet, CommandPacket& output, ParseError& error) {
    std::array<std::string_view, 6> fields{};
    if (!SplitExact(packet, fields)) {
        error = ParseError::FieldCount;
        return false;
    }
    if (fields[0] != kVersion1 && fields[0] != kVersion2) {
        error = ParseError::Version;
        return false;
    }
    if (fields[1] != "CMD") {
        error = ParseError::PacketType;
        return false;
    }

    CommandPacket parsed{};
    if (fields[2] == "START" && fields[0] == kVersion1) {
        parsed.type = CommandType::Start;
    } else if (fields[2] == "START_AT" && fields[0] == kVersion2) {
        parsed.type = CommandType::StartAt;
    } else if (fields[2] == "RESET" && fields[0] == kVersion1) {
        parsed.type = CommandType::Reset;
    } else if (fields[2] == "STATUS_REQUEST" && fields[0] == kVersion1) {
        parsed.type = CommandType::StatusRequest;
    } else {
        error = ParseError::CommandType;
        return false;
    }

    if (!ParseCommandId(fields[3], parsed.command_id)) {
        error = ParseError::CommandId;
        return false;
    }

    if (!ParseDecimal(fields[4], parsed.duration_seconds) ||
        (parsed.type == CommandType::StatusRequest && parsed.duration_seconds != 0) ||
        (parsed.type != CommandType::StatusRequest &&
         (parsed.duration_seconds < kMinDurationSeconds || parsed.duration_seconds > kMaxDurationSeconds))) {
        error = ParseError::Duration;
        return false;
    }

    if (parsed.type == CommandType::StartAt) {
        if (!ParseDecimal(fields[5], parsed.start_at_master_us) || parsed.start_at_master_us <= 0) {
            error = ParseError::StartAt;
            return false;
        }
    } else {
        if (!ParseDecimal(fields[5], parsed.start_delay_ms)) {
            error = ParseError::StartDelay;
            return false;
        }
        if ((parsed.type == CommandType::Start &&
             (parsed.start_delay_ms < kMinStartDelayMs || parsed.start_delay_ms > kMaxStartDelayMs)) ||
            ((parsed.type == CommandType::Reset || parsed.type == CommandType::StatusRequest) &&
             parsed.start_delay_ms != 0)) {
            error = ParseError::StartDelay;
            return false;
        }
    }

    output = parsed;
    return true;
}

}  // namespace

bool ParseMasterPacket(std::string_view packet, MasterPacket& output, ParseError& error) {
    if (!ValidateEnvelope(packet, error)) return false;

    if (packet.rfind("FCT1|CMD|", 0) == 0 || packet.rfind("FCT2|CMD|", 0) == 0) {
        CommandPacket command{};
        if (!ParseCommandFields(packet, command, error)) return false;
        output = {};
        output.type = MasterPacketType::Command;
        output.command = command;
        return true;
    }

    if (packet.rfind("FCT2|SYNC|", 0) == 0) {
        // Backward compatible forms:
        //   FCT2|SYNC|<id>|<t1>
        //   FCT2|SYNC|<id>|<t1>|<artificial_reply_delay_us>
        // The optional reply delay is used only by the controlled path-delay
        // experiment. t3 is captured before that delay so NTP-style math sees
        // it as reverse-path delay rather than device processing time.
        std::string_view id_field;
        std::string_view t1_field;
        std::string_view delay_field;
        bool has_delay = false;

        std::array<std::string_view, 5> fields5{};
        if (SplitExact(packet, fields5)) {
            id_field = fields5[2];
            t1_field = fields5[3];
            delay_field = fields5[4];
            has_delay = true;
        } else {
            std::array<std::string_view, 4> fields4{};
            if (!SplitExact(packet, fields4)) {
                error = ParseError::FieldCount;
                return false;
            }
            id_field = fields4[2];
            t1_field = fields4[3];
        }

        uint64_t sync_id = 0;
        int64_t t1 = 0;
        uint32_t artificial_reply_delay_us = 0;
        if (!ParseCommandId(id_field, sync_id)) {
            error = ParseError::SyncId;
            return false;
        }
        if (!ParseDecimal(t1_field, t1) || t1 < 0) {
            error = ParseError::Timestamp;
            return false;
        }
        if (has_delay &&
            (!ParseDecimal(delay_field, artificial_reply_delay_us) ||
             artificial_reply_delay_us > kMaxSyncArtificialReplyDelayUs)) {
            error = ParseError::Delay;
            return false;
        }
        output = {};
        output.type = MasterPacketType::SyncRequest;
        output.sync_request = SyncRequestPacket{sync_id, t1, artificial_reply_delay_us};
        return true;
    }

    if (packet.rfind("FCT2|SYNC_SET|", 0) == 0) {
        std::array<std::string_view, 5> fields{};
        if (!SplitExact(packet, fields)) {
            error = ParseError::FieldCount;
            return false;
        }
        uint64_t sync_id = 0;
        int64_t offset = 0;
        uint64_t rtt = 0;
        if (!ParseCommandId(fields[2], sync_id)) {
            error = ParseError::SyncId;
            return false;
        }
        if (!ParseDecimal(fields[3], offset)) {
            error = ParseError::Offset;
            return false;
        }
        if (!ParseDecimal(fields[4], rtt)) {
            error = ParseError::Rtt;
            return false;
        }
        output = {};
        output.type = MasterPacketType::SyncSet;
        output.sync_set = SyncSetPacket{sync_id, offset, rtt};
        return true;
    }

    error = packet.rfind("FCT2|", 0) == 0 || packet.rfind("FCT1|", 0) == 0
        ? ParseError::PacketType
        : ParseError::Version;
    return false;
}

bool ParseCommand(std::string_view packet, CommandPacket& output, ParseError& error) {
    if (!ValidateEnvelope(packet, error)) return false;
    return ParseCommandFields(packet, output, error);
}

const char* ParseErrorName(ParseError error) {
    switch (error) {
        case ParseError::None: return "none";
        case ParseError::Empty: return "empty packet";
        case ParseError::TooLong: return "packet too long";
        case ParseError::NonPrintable: return "non-printable input";
        case ParseError::FieldCount: return "wrong field count";
        case ParseError::Version: return "unsupported version";
        case ParseError::PacketType: return "unsupported packet type";
        case ParseError::CommandType: return "unsupported command type";
        case ParseError::CommandId: return "invalid command ID";
        case ParseError::Duration: return "invalid duration";
        case ParseError::StartDelay: return "invalid start delay";
        case ParseError::StartAt: return "invalid absolute start timestamp";
        case ParseError::SyncId: return "invalid sync ID";
        case ParseError::Timestamp: return "invalid timestamp";
        case ParseError::Offset: return "invalid clock offset";
        case ParseError::Rtt: return "invalid RTT";
        case ParseError::Delay: return "invalid artificial delay";
    }
    return "unknown";
}

const char* CommandTypeName(CommandType type) {
    switch (type) {
        case CommandType::Start: return "START";
        case CommandType::StartAt: return "START_AT";
        case CommandType::Reset: return "RESET";
        case CommandType::StatusRequest: return "STATUS_REQUEST";
    }
    return "STATUS_REQUEST";
}

const char* TimerStateName(TimerState state) {
    switch (state) {
        case TimerState::Ready: return "READY";
        case TimerState::Armed: return "ARMED";
        case TimerState::Running: return "RUNNING";
        case TimerState::Finished: return "FINISHED";
    }
    return "READY";
}

int FormatAck(char* destination, std::size_t capacity, std::string_view device_id,
              const CommandPacket& command, AckResult result) {
    if (destination == nullptr || capacity == 0 || !ValidDeviceId(device_id)) return -1;
    const char* result_text = "ACCEPTED";
    switch (result) {
        case AckResult::Accepted: result_text = "ACCEPTED"; break;
        case AckResult::Duplicate: result_text = "DUPLICATE"; break;
        case AckResult::NotSynced: result_text = "NOT_SYNCED"; break;
        case AckResult::Late: result_text = "LATE"; break;
    }
    return std::snprintf(destination, capacity, "FCT1|ACK|%.*s|%016llX|%s|%s",
                         static_cast<int>(device_id.size()), device_id.data(),
                         static_cast<unsigned long long>(command.command_id),
                         CommandTypeName(command.type), result_text);
}

int FormatStatus(char* destination, std::size_t capacity, std::string_view device_id,
                 uint64_t command_id, TimerState state, uint32_t remaining_seconds) {
    if (destination == nullptr || capacity == 0 || !ValidDeviceId(device_id)) return -1;
    return std::snprintf(destination, capacity, "FCT1|STATUS|%.*s|%016llX|%s|%u",
                         static_cast<int>(device_id.size()), device_id.data(),
                         static_cast<unsigned long long>(command_id), TimerStateName(state),
                         static_cast<unsigned>(remaining_seconds));
}

int FormatSyncReply(char* destination, std::size_t capacity, std::string_view device_id,
                    uint64_t sync_id, int64_t master_t1_us,
                    int64_t local_t2_us, int64_t local_t3_us) {
    if (destination == nullptr || capacity == 0 || !ValidDeviceId(device_id) || sync_id == 0) return -1;
    return std::snprintf(destination, capacity,
                         "FCT2|SYNC_REPLY|%.*s|%016llX|%lld|%lld|%lld",
                         static_cast<int>(device_id.size()), device_id.data(),
                         static_cast<unsigned long long>(sync_id),
                         static_cast<long long>(master_t1_us),
                         static_cast<long long>(local_t2_us),
                         static_cast<long long>(local_t3_us));
}

int FormatSyncApplied(char* destination, std::size_t capacity, std::string_view device_id,
                      uint64_t sync_id, int64_t master_minus_local_offset_us,
                      uint64_t best_rtt_us) {
    if (destination == nullptr || capacity == 0 || !ValidDeviceId(device_id) || sync_id == 0) return -1;
    return std::snprintf(destination, capacity,
                         "FCT2|SYNC_APPLIED|%.*s|%016llX|%lld|%llu",
                         static_cast<int>(device_id.size()), device_id.data(),
                         static_cast<unsigned long long>(sync_id),
                         static_cast<long long>(master_minus_local_offset_us),
                         static_cast<unsigned long long>(best_rtt_us));
}

int FormatStarted(char* destination, std::size_t capacity, std::string_view device_id,
                  uint64_t command_id, int64_t local_start_us,
                  int64_t estimated_master_start_us, int64_t target_master_start_us) {
    if (destination == nullptr || capacity == 0 || !ValidDeviceId(device_id) || command_id == 0) return -1;
    return std::snprintf(destination, capacity,
                         "FCT2|STARTED|%.*s|%016llX|%lld|%lld|%lld",
                         static_cast<int>(device_id.size()), device_id.data(),
                         static_cast<unsigned long long>(command_id),
                         static_cast<long long>(local_start_us),
                         static_cast<long long>(estimated_master_start_us),
                         static_cast<long long>(target_master_start_us));
}

}  // namespace factory_timer
