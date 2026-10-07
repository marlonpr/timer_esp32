#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace factory_timer {

constexpr std::size_t kMaxPacketLength = 511;
constexpr uint32_t kMinDurationSeconds = 1;
constexpr uint32_t kMaxDurationSeconds = 86400;
constexpr uint32_t kMinStartDelayMs = 100;
constexpr uint32_t kMaxStartDelayMs = 10000;
constexpr uint32_t kMaxSyncArtificialReplyDelayUs = 1500000;

enum class CommandType { Start, StartAt, Reset, StatusRequest, Brightness };
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
    Brightness,
};

struct CommandPacket {
    CommandType type{};
    uint64_t command_id{};
    uint32_t duration_seconds{};
    uint32_t start_delay_ms{};
    int64_t start_at_master_us{};
    uint8_t brightness_percent{};
};

struct SyncRequestPacket {
    uint64_t sync_id{};
    int64_t master_t1_us{};
    uint32_t artificial_reply_delay_us{};
    bool request_die_temperature{};
};

struct SyncSetPacket {
    uint64_t sync_id{};
    int64_t master_minus_local_offset_us{};
    uint64_t best_rtt_us{};
    int64_t offset_epoch_local_us{};
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
                 uint64_t command_id, TimerState state, uint32_t remaining_seconds,
                 int rssi_dbm, uint8_t wifi_channel, std::string_view bssid,
                 std::string_view rtc_discipline_state, uint16_t rtc_fit_points,
                 double rtc_fit_rms_us, uint32_t rtc_queue_drops,
                 bool rtc_temperature_valid, double rtc_rate_ppm_vs_rtc,
                 uint64_t rtc_fit_outliers, double rtc_temperature_c,
                 int rtc_sqw_core, uint8_t health_flags,
                 int64_t sync_source_offset_us, int64_t sync_epoch_local_us,
                 int64_t sync_epoch_disciplined_us,
                 int64_t sync_master_minus_disciplined_us,
                 int64_t start_error_us, int64_t scheduler_lateness_us,
                 int64_t start_publish_lateness_us,
                 int64_t worst_publish_lateness_us,
                 uint32_t frame_not_ready_count, uint64_t rtc_accepted_edges,
                 uint64_t rtc_inferred_missing_edges, uint64_t rtc_holdover_entries,
                 bool cpu0_monitor_valid = false,
                 uint32_t cpu0_monitor_samples = 0,
                 uint32_t cpu0_monitor_event_count = 0,
                 uint32_t cpu0_monitor_worst_us = 0,
                 std::string_view cpu0_monitor_worst_task = "NONE",
                 uint32_t cpu0_commit_late_count = 0,
                 uint32_t cpu0_commit_worst_us = 0,
                 bool cpu0_commit_overlap = false,
                 uint32_t cpu0_wrong_core_callbacks = 0,
                 uint32_t cpu0_monitor_overflow = 0,
                 bool cpu0_interrupt_level_match = false,
                 uint32_t cpu0_monitor_missed_periods = 0,
                 std::string_view firmware_elf_sha8 = "00000000");
struct RunDiagnosticFields {
    bool valid{};
    uint32_t period_us{};
    uint32_t threshold_us{};
    int64_t monitor_start_us{};
    int64_t tstar_local_us{};
    int64_t monitor_end_us{};
    uint32_t first_alarm_offset_us{};
    uint64_t expected_periods{};
    uint32_t sample_callbacks{};
    uint32_t missed_periods{};
    uint32_t cpu0_events_ge_50us{};
    uint32_t rtc_discipline_events_ge_50us{};
    uint32_t wifi_events_ge_50us{};
    uint32_t udp_events_ge_50us{};
    uint32_t commit_late_events{};
    uint32_t commit_ge_300us{};
    uint32_t rearm_failures{};
};
int FormatRunDiagnostic(char* destination, std::size_t capacity,
                        std::string_view device_id, uint64_t command_id,
                        const RunDiagnosticFields& fields);

int FormatSyncReply(char* destination, std::size_t capacity, std::string_view device_id,
                    uint64_t sync_id, int64_t master_t1_us,
                    int64_t local_t2_us, int64_t local_t3_us,
                    int64_t ingress_local_us,
                    uint32_t actual_artificial_reply_delay_us = 0,
                    bool has_die_temperature = false,
                    int32_t die_temperature_milli_c = 0);
int FormatSyncApplied(char* destination, std::size_t capacity, std::string_view device_id,
                      uint64_t sync_id, int64_t master_minus_local_offset_us,
                      uint64_t best_rtt_us, int64_t offset_epoch_local_us);
int FormatStarted(char* destination, std::size_t capacity, std::string_view device_id,
                  uint64_t command_id, int64_t local_start_us,
                  int64_t estimated_master_start_us, int64_t target_master_start_us);

}  // namespace factory_timer
