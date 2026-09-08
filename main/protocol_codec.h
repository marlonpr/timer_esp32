#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace factory_timer {

constexpr std::size_t kMaxPacketLength = 191;
constexpr uint32_t kMinDurationSeconds = 1;
constexpr uint32_t kMaxDurationSeconds = 86400;
constexpr uint32_t kMinStartDelayMs = 100;
constexpr uint32_t kMaxStartDelayMs = 10000;
constexpr uint32_t kMaxSyncArtificialReplyDelayUs = 1500000;

enum class CommandType { Start, StartAt, Reset, StatusRequest };
enum class TimerState { Ready, Armed, Running, Finished };
enum class AckResult { Accepted, Duplicate, NotSynced, Late };

enum class MasterPacketType { Command, SyncRequest, SyncSet };

enum class ParseError {
    None,
    Empty,
    TooLong,
    NonPrintable,
    FieldCount,
    Version,
    PacketType,
    CommandType,
    CommandId,
    Duration,
    StartDelay,
    StartAt,
    SyncId,
    Timestamp,
    Offset,
    Rtt,
    Delay,
};

struct CommandPacket {
    CommandType type{};
    uint64_t command_id{};
    uint32_t duration_seconds{};
    uint32_t start_delay_ms{};
    int64_t start_at_master_us{};
};

struct SyncRequestPacket {
    uint64_t sync_id{};
    int64_t master_t1_us{};
    uint32_t artificial_reply_delay_us{};
};

struct SyncSetPacket {
    uint64_t sync_id{};
    int64_t master_minus_local_offset_us{};
    uint64_t best_rtt_us{};
};

struct MasterPacket {
    MasterPacketType type{};
    CommandPacket command{};
    SyncRequestPacket sync_request{};
    SyncSetPacket sync_set{};
};

bool ParseMasterPacket(std::string_view packet, MasterPacket& output, ParseError& error);
bool ParseCommand(std::string_view packet, CommandPacket& output, ParseError& error);
const char* ParseErrorName(ParseError error);
const char* CommandTypeName(CommandType type);
const char* TimerStateName(TimerState state);

int FormatAck(char* destination, std::size_t capacity, std::string_view device_id,
              const CommandPacket& command, AckResult result);
int FormatStatus(char* destination, std::size_t capacity, std::string_view device_id,
                 uint64_t command_id, TimerState state, uint32_t remaining_seconds);
int FormatSyncReply(char* destination, std::size_t capacity, std::string_view device_id,
                    uint64_t sync_id, int64_t master_t1_us,
                    int64_t local_t2_us, int64_t local_t3_us);
int FormatSyncApplied(char* destination, std::size_t capacity, std::string_view device_id,
                      uint64_t sync_id, int64_t master_minus_local_offset_us,
                      uint64_t best_rtt_us);
int FormatStarted(char* destination, std::size_t capacity, std::string_view device_id,
                  uint64_t command_id, int64_t local_start_us,
                  int64_t estimated_master_start_us, int64_t target_master_start_us);

}  // namespace factory_timer
