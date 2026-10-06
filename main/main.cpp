#include "command_processor.h"
#include "factory_log.h"
#include "factory_display.h"
#include "flash_guard_diag.h"
#include "network_policy.h"
#include "protocol_codec.h"
#include "ds3231.h"
#include "rtc_discipline.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "driver/gpio.h"
#if CONFIG_IDF_TARGET_ESP32S3
#include "driver/temperature_sensor.h"
#endif
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/udp.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if defined(CONFIG_FACTORY_DS3231_DISCIPLINE) && CONFIG_FACTORY_DS3231_DISCIPLINE
#if CONFIG_FACTORY_DS3231_SQW_GPIO == DS3231_SDA_PIN || CONFIG_FACTORY_DS3231_SQW_GPIO == DS3231_SCL_PIN
#error "DS3231 SQW GPIO must not overlap SDA GPIO21 or SCL GPIO22"
#endif
#if defined(CONFIG_FACTORY_START_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_START_EDGE_DIAGNOSTICS
#if CONFIG_FACTORY_START_EDGE_GPIO == DS3231_SDA_PIN || CONFIG_FACTORY_START_EDGE_GPIO == DS3231_SCL_PIN
#error "START diagnostic GPIO must not overlap the DS3231 I2C bus"
#endif
#if CONFIG_FACTORY_START_EDGE_GPIO == CONFIG_FACTORY_DS3231_SQW_GPIO
#error "START diagnostic GPIO must not overlap DS3231 SQW"
#endif
#endif
#if defined(CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_EDGE_DIAGNOSTICS
#if CONFIG_FACTORY_DISPLAY_EDGE_GPIO == DS3231_SDA_PIN || CONFIG_FACTORY_DISPLAY_EDGE_GPIO == DS3231_SCL_PIN
#error "Display diagnostic GPIO must not overlap the DS3231 I2C bus"
#endif
#if CONFIG_FACTORY_DISPLAY_EDGE_GPIO == CONFIG_FACTORY_DS3231_SQW_GPIO
#error "Display diagnostic GPIO must not overlap DS3231 SQW"
#endif
#if defined(CONFIG_FACTORY_START_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_START_EDGE_DIAGNOSTICS
#if CONFIG_FACTORY_DISPLAY_EDGE_GPIO == CONFIG_FACTORY_START_EDGE_GPIO
#error "Display diagnostic GPIO must not overlap START diagnostic GPIO"
#endif
#endif
#endif
#endif

#if defined(CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_DIAGNOSTICS
#if CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_GPIO == CONFIG_FACTORY_DISPLAY_EDGE_GPIO
#error "Refresh adoption marker GPIO must not overlap display commit marker GPIO"
#endif
#if defined(CONFIG_FACTORY_START_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_START_EDGE_DIAGNOSTICS
#if CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_GPIO == CONFIG_FACTORY_START_EDGE_GPIO
#error "Refresh adoption marker GPIO must not overlap START diagnostic GPIO"
#endif
#endif
#if defined(CONFIG_FACTORY_DS3231_DISCIPLINE) && CONFIG_FACTORY_DS3231_DISCIPLINE
#if CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_GPIO == DS3231_SDA_PIN || CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_GPIO == DS3231_SCL_PIN
#error "Refresh adoption marker GPIO must not overlap DS3231 I2C"
#endif
#if CONFIG_FACTORY_DISPLAY_REFRESH_EDGE_GPIO == CONFIG_FACTORY_DS3231_SQW_GPIO
#error "Refresh adoption marker GPIO must not overlap DS3231 SQW"
#endif
#endif
#endif

namespace {

constexpr char kTag[] = "factory_timer";
constexpr uint16_t kCommandPort = 5000;
constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr int64_t kHeartbeatIntervalUs = 2000000;
constexpr int64_t kCommandSocketTimeoutUs = 50000;
constexpr uint32_t kRejectedNetworkRetryDelayMs = 1000;
constexpr UBaseType_t kTimerTaskPriority = 24;
constexpr UBaseType_t kCommandTaskPriority = 15;
constexpr UBaseType_t kRtcQualificationTaskPriority = 5;
constexpr UBaseType_t kRxBoundaryTraceTaskPriority = 4;
constexpr UBaseType_t kDiagnosticDumpTaskPriority = 1;
constexpr BaseType_t kControlTaskCore = 0;
constexpr int64_t kSchedulerFineLeadUs = 2000;
constexpr int64_t kFreeRtosTickUs = 1000000LL / configTICK_RATE_HZ;
constexpr int64_t kExperimentalDelayFineLeadUs = 2000;
constexpr int64_t kRxBoundaryTraceWindowUs = 15000;
constexpr size_t kRxBoundaryTraceCapacity = 64;
constexpr size_t kStatusPathTraceCapacity = 64;
constexpr size_t kStatusTxTraceCapacity = 128;
constexpr size_t kSyncReplyTraceCapacity = 64;
constexpr size_t kEarlyIngressCapacity = 128;

std::string_view FirmwareElfSha8() {
    static char sha8[9]{};
    static bool initialized = false;
    if (!initialized) {
        (void)esp_app_get_elf_sha256(sha8, sizeof(sha8));
        if (std::strlen(sha8) != 8) {
            std::snprintf(sha8, sizeof(sha8), "00000000");
        }
        initialized = true;
    }
    return std::string_view(sha8, 8);
}

EventGroupHandle_t wifi_event_group;
esp_netif_t *wifi_sta_netif = nullptr;
TaskHandle_t dhcp_retry_task_handle = nullptr;
std::atomic<bool> dhcp_retry_pending{false};
std::atomic<uint32_t> rejected_network_count{0};
std::atomic<uint32_t> running_heartbeat_suppressed{0};
factory_timer::CountdownTimer countdown;
SemaphoreHandle_t countdown_mutex = nullptr;
QueueHandle_t timer_event_queue = nullptr;
TaskHandle_t timer_task_handle = nullptr;
TaskHandle_t diagnostic_dump_task_handle = nullptr;
ds3231_dev_t ds3231_device{};
bool rtc_discipline_ready = false;

#if CONFIG_IDF_TARGET_ESP32S3
temperature_sensor_handle_t die_temperature_sensor = nullptr;
bool die_temperature_sensor_ready = false;
#endif

struct ClockSyncState {
    std::atomic<bool> valid{false};
    int64_t master_minus_local_offset_us{};
    uint64_t best_rtt_us{};
    uint64_t sync_id{};
    int64_t applied_local_us{};
    int64_t estimated_master_apply_us{};
    int64_t source_offset_us{};
    int64_t offset_epoch_local_us{};
    int64_t offset_epoch_disciplined_us{};
    int64_t estimated_master_epoch_us{};
};

ClockSyncState clock_sync;

struct StartHealthState {
    bool valid{};
    uint64_t command_id{};
    int64_t start_error_us{};
    int64_t scheduler_lateness_us{};
};

StartHealthState last_start_health;

struct ScheduledStartMetadata {
    bool valid{};
    sockaddr_in peer{};
    bool have_peer{};
    uint64_t command_id{};
    int64_t target_master_start_us{};
    int64_t master_minus_local_offset_us{};
    uint64_t sync_id{};
    uint64_t best_rtt_us{};
    int64_t sync_applied_local_us{};
    int64_t sync_estimated_master_us{};
    int64_t source_offset_us{};
    int64_t offset_epoch_local_us{};
    int64_t offset_epoch_disciplined_us{};
    int64_t estimated_master_epoch_us{};
};

struct TimerStartEvent {
    factory_timer::TimerSnapshot snapshot{};
    sockaddr_in peer{};
    bool have_peer{};
    int64_t actual_local_start_us{};
    int64_t actual_disciplined_start_us{};
    int64_t target_local_start_us{};
    int64_t estimated_master_start_us{-1};
    int64_t target_master_start_us{};
    int64_t master_minus_local_offset_us{};
    uint64_t sync_id{};
    uint64_t best_rtt_us{};
    int64_t sync_applied_local_us{};
    int64_t sync_estimated_master_us{};
    int64_t source_offset_us{};
    int64_t offset_epoch_local_us{};
    int64_t offset_epoch_disciplined_us{};
    int64_t estimated_master_epoch_us{};
};

struct StatusSendTiming {
    int64_t send_status_begin_us{};
    int64_t wifi_diag_begin_us{};
    int64_t wifi_diag_end_us{};
    int64_t rtc_state_begin_us{};
    int64_t rtc_state_end_us{};
    int64_t format_done_us{};
    int64_t sendto_entry_us{};
    int64_t sendto_return_us{};
    int64_t send_status_end_us{};
};

struct EarlyIngressRecord {
    int64_t ip_input_us{};
    uint32_t source_address{};
    uint16_t source_port{};
    uint16_t destination_port{};
    uint16_t payload_length{};
    uint64_t payload_fingerprint{};
    uint32_t sequence{};
    bool consumed{};
};

EarlyIngressRecord early_ingress_ring[kEarlyIngressCapacity]{};
portMUX_TYPE early_ingress_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t early_ingress_write_count = 0;
uint32_t early_ingress_overwrites = 0;

constexpr uint64_t kIngressFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kIngressFnvPrime = 1099511628211ULL;

uint64_t PayloadFingerprint(const void *data, size_t length) {
    const auto *bytes = static_cast<const uint8_t *>(data);
    uint64_t hash = kIngressFnvOffsetBasis;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= kIngressFnvPrime;
    }
    return hash;
}

uint64_t PbufPayloadFingerprint(const struct pbuf *p, uint16_t offset, uint16_t length) {
    uint64_t hash = kIngressFnvOffsetBasis;
    uint8_t chunk[32];
    uint16_t copied_total = 0;
    while (copied_total < length) {
        const uint16_t want = static_cast<uint16_t>(
            ((length - copied_total) < sizeof(chunk)) ? (length - copied_total) : sizeof(chunk));
        const u16_t copied = pbuf_copy_partial(
            p, chunk, want, static_cast<u16_t>(offset + copied_total));
        if (copied != want) return 0;
        for (uint16_t i = 0; i < want; ++i) {
            hash ^= chunk[i];
            hash *= kIngressFnvPrime;
        }
        copied_total = static_cast<uint16_t>(copied_total + want);
    }
    return hash;
}

void RecordEarlyIngress(int64_t ip_input_us, uint32_t source_address,
                        uint16_t source_port, uint16_t destination_port,
                        uint16_t payload_length, uint64_t payload_fingerprint) {
    portENTER_CRITICAL(&early_ingress_mux);
    const uint32_t sequence = ++early_ingress_write_count;
    const size_t index = (sequence - 1u) % kEarlyIngressCapacity;
    if (sequence > kEarlyIngressCapacity && !early_ingress_ring[index].consumed) {
        ++early_ingress_overwrites;
    }
    EarlyIngressRecord record{};
    record.ip_input_us = ip_input_us;
    record.source_address = source_address;
    record.source_port = source_port;
    record.destination_port = destination_port;
    record.payload_length = payload_length;
    record.payload_fingerprint = payload_fingerprint;
    record.sequence = sequence;
    record.consumed = false;
    early_ingress_ring[index] = record;
    portEXIT_CRITICAL(&early_ingress_mux);
}

int64_t MatchEarlyIngress(const sockaddr_in &peer, const void *payload, int received_bytes,
                          uint32_t *matched_sequence = nullptr) {
    if (payload == nullptr || received_bytes <= 0) return -1;
    const uint64_t payload_fingerprint =
        PayloadFingerprint(payload, static_cast<size_t>(received_bytes));
    int64_t result = -1;
    uint32_t best_sequence = UINT32_MAX;
    portENTER_CRITICAL(&early_ingress_mux);
    for (size_t i = 0; i < kEarlyIngressCapacity; ++i) {
        EarlyIngressRecord &r = early_ingress_ring[i];
        if (r.sequence == 0 || r.consumed ||
            r.source_address != peer.sin_addr.s_addr ||
            r.source_port != ntohs(peer.sin_port) ||
            r.destination_port != kCommandPort ||
            r.payload_length != static_cast<uint16_t>(received_bytes) ||
            r.payload_fingerprint != payload_fingerprint) {
            continue;
        }
        if (r.sequence < best_sequence) {
            best_sequence = r.sequence;
            result = r.ip_input_us;
        }
    }
    if (best_sequence != UINT32_MAX) {
        const size_t index = (best_sequence - 1u) % kEarlyIngressCapacity;
        if (early_ingress_ring[index].sequence == best_sequence) {
            early_ingress_ring[index].consumed = true;
        }
        if (matched_sequence != nullptr) *matched_sequence = best_sequence;
    }
    portEXIT_CRITICAL(&early_ingress_mux);
    return result;
}

struct SyncReplyTraceRecord {
    uint64_t sync_id{};
    int64_t master_t1_us{};
    int64_t ip_input_us{-1};
    int64_t local_t2_us{};
    int64_t local_t3_us{};
    int64_t format_done_us{};
    int64_t sendto_entry_us{};
    int64_t sendto_return_us{};
    uint32_t requested_delay_us{};
    uint32_t reported_hold_us{};
    uint16_t packet_length{};
    bool sent{};
    bool selected_by_sync_set{};
    bool t3_late_patch{};
};

SyncReplyTraceRecord sync_reply_trace[kSyncReplyTraceCapacity]{};
size_t sync_reply_trace_count = 0;
bool sync_reply_trace_overflow = false;
uint64_t sync_reply_trace_sync_id = 0;

#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
struct RxBoundaryEvent {
    int64_t receive_local_us{};
    int64_t receive_disciplined_us{};
    uint32_t boundary_index{};
    int32_t lead_us{};
    uint32_t source_address{};
    uint16_t source_port{};
    bool parsed{};
    factory_timer::MasterPacketType packet_type{};
    factory_timer::CommandType command_type{};
};

enum class StatusSendReason : uint8_t {
    RequestReply,
    TimerStarted,
    StateChange,
    Heartbeat,
    Other,
};

struct StatusPathTraceRecord {
    uint64_t command_id{};
    uint32_t source_address{};
    uint16_t source_port{};
    int64_t ip_input_us{-1};
    int64_t recv_return_us{};
    int64_t parse_done_us{};
    int64_t rx_trace_begin_us{};
    int64_t rx_trace_end_us{};
    int64_t lock_request_us{};
    int64_t lock_acquired_us{};
    int64_t discipline_begin_us{};
    int64_t discipline_end_us{};
    int64_t process_done_us{};
    int64_t lock_release_us{};
    int64_t handler_exit_us{};
    int64_t receive_disciplined_us{};
    uint32_t boundary_index{};
    int32_t lead_us{};
    factory_timer::TimerState snapshot_state{factory_timer::TimerState::Ready};
    StatusSendTiming send{};
};

RxBoundaryEvent rx_boundary_trace[kRxBoundaryTraceCapacity]{};
size_t rx_boundary_trace_count = 0;
bool rx_boundary_trace_overflow = false;
struct StatusTxTraceRecord {
    StatusSendReason reason{StatusSendReason::Other};
    factory_timer::TimerState state{factory_timer::TimerState::Ready};
    uint32_t remaining_seconds{};
    StatusSendTiming send{};
};

StatusPathTraceRecord status_path_trace[kStatusPathTraceCapacity]{};
size_t status_path_trace_count = 0;
bool status_path_trace_overflow = false;
StatusTxTraceRecord status_tx_trace[kStatusTxTraceCapacity]{};
size_t status_tx_trace_count = 0;
bool status_tx_trace_overflow = false;
#endif

ScheduledStartMetadata scheduled_start_metadata;

bool ValidConfiguredDeviceId(std::string_view value) {
    if (value.empty() || value.size() > 16) return false;
    for (const char character : value) {
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '_' ||
              character == '-')) {
            return false;
        }
    }
    return true;
}

bool IsAcceptedIpInfo(const esp_netif_ip_info_t &info) {
    return factory_timer::IsAcceptedFactoryNetwork(
        ntohl(info.ip.addr), ntohl(info.netmask.addr), ntohl(info.gw.addr));
}

void RequestDhcpRetry() {
    if (dhcp_retry_task_handle == nullptr) return;
    if (!dhcp_retry_pending.exchange(true)) {
        xTaskNotifyGive(dhcp_retry_task_handle);
    }
}

void DhcpRetryTask(void *) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(kRejectedNetworkRetryDelayMs));

        if (wifi_sta_netif == nullptr) {
            ESP_LOGE(kTag, "Cannot retry DHCP because the station netif is unavailable");
            dhcp_retry_pending.store(false);
            continue;
        }

        ESP_LOGW(kTag,
                 "Retrying DHCP after rejected network lease (attempt=%u)",
                 static_cast<unsigned>(rejected_network_count.load()));

        const esp_err_t stop_result = esp_netif_dhcpc_stop(wifi_sta_netif);
        if (stop_result != ESP_OK) {
            FACTORY_DEBUG_LOGI(kTag, "DHCP client stop returned %d",
                               static_cast<int>(stop_result));
        }

        // Clear the rejected address while DHCP is stopped so no application
        // traffic can accidentally use it during the retry.
        const esp_netif_ip_info_t empty_ip_info{};
        const esp_err_t clear_result =
            esp_netif_set_ip_info(wifi_sta_netif, &empty_ip_info);
        if (clear_result != ESP_OK) {
            ESP_LOGW(kTag, "Could not clear rejected IPv4 lease: error=%d",
                     static_cast<int>(clear_result));
        }

        // Clear before restarting DHCP so a new rejected GOT_IP event can queue
        // the next retry immediately.
        dhcp_retry_pending.store(false);
        const esp_err_t start_result = esp_netif_dhcpc_start(wifi_sta_netif);
        if (start_result != ESP_OK) {
            ESP_LOGW(kTag, "DHCP client restart failed: error=%d; retrying",
                     static_cast<int>(start_result));
            RequestDhcpRetry();
        }
    }
}

void InitialiseRtcDiscipline() {
#if defined(CONFIG_FACTORY_DS3231_DISCIPLINE) && CONFIG_FACTORY_DS3231_DISCIPLINE
    esp_err_t result = init_ds3231(&ds3231_device);
    if (result != ESP_OK) {
        ESP_LOGE(kTag,
                 "DS3231 initialization failed (%s); disciplined timing is unavailable",
                 esp_err_to_name(result));
        return;
    }

    rtc_discipline_config_t config{};
    config.sqw_gpio = static_cast<gpio_num_t>(CONFIG_FACTORY_DS3231_SQW_GPIO);
    config.enable_internal_pullup = true;
    config.acquire_points = 33;
    config.fit_points = 129;
    config.holdover_timeout_ms = 3500;
    config.max_inferred_gap_s = 8;

    result = rtc_discipline_init(&ds3231_device, &config);
    if (result != ESP_OK) {
        ESP_LOGE(kTag,
                 "DS3231 discipline initialization failed (%s); disciplined timing is unavailable",
                 esp_err_to_name(result));
        return;
    }

    rtc_discipline_ready = true;
    ESP_LOGI(kTag,
             "DS3231 discipline active: SDA=GPIO%d SCL=GPIO%d SQW=GPIO%d acquisition=32s fit_window=128s",
             DS3231_SDA_PIN, DS3231_SCL_PIN, CONFIG_FACTORY_DS3231_SQW_GPIO);
#else
    ESP_LOGW(kTag, "DS3231 discipline disabled by configuration");
#endif
}

#if defined(CONFIG_FACTORY_RTC_QUAL_LOGS) && CONFIG_FACTORY_RTC_QUAL_LOGS
void RtcQualificationTask(void *) {
    const TickType_t period_ticks =
        pdMS_TO_TICKS(CONFIG_FACTORY_RTC_QUAL_LOG_INTERVAL_S * 1000U);
    TickType_t last_wake = xTaskGetTickCount();

    ESP_LOGI(kTag,
             "RTC qualification logging enabled: interval_s=%u task_priority=%u core=%d",
             static_cast<unsigned>(CONFIG_FACTORY_RTC_QUAL_LOG_INTERVAL_S),
             static_cast<unsigned>(kRtcQualificationTaskPriority),
             static_cast<int>(kControlTaskCore));

    while (true) {
        vTaskDelayUntil(&last_wake, period_ticks);

        if (!rtc_discipline_ready) {
            continue;
        }

        const int64_t local_us = esp_timer_get_time();
        const int64_t disciplined_us =
            rtc_discipline_local_to_disciplined_us(local_us);
        rtc_discipline_status_t status{};
        rtc_discipline_get_status(&status);

        ESP_LOGI(kTag,
                 "RTC_QUAL device=%s local_us=%lld disciplined_us=%lld state=%s rate_ppm=%+.6f rms_us=%.3f points=%u accepted=%llu rejected=%llu fit_outliers=%llu fit_last_outlier_us=%+.3f fit_max_outlier_us=%.3f inferred_missing=%llu holdover_entries=%llu queue_drops=%u temp_valid=%u temp_c=%.2f sqw_core=%d sqw_n=%u sqw_last_period_us=%lld sqw_period_mean_us=%.3f sqw_period_rms_us=%.3f sqw_period_p2p_us=%.3f sqw_trace_n=%u sqw_trace_overwrites=%u",
                 CONFIG_FACTORY_DEVICE_ID,
                 static_cast<long long>(local_us),
                 static_cast<long long>(disciplined_us),
                 rtc_discipline_state_name(status.state),
                 status.local_rate_ppm_vs_rtc,
                 status.fit_rms_us,
                 static_cast<unsigned>(status.fit_point_count),
                 static_cast<unsigned long long>(status.accepted_edges),
                 static_cast<unsigned long long>(status.rejected_edges),
                 static_cast<unsigned long long>(status.fit_outlier_edges),
                 status.fit_last_outlier_residual_us,
                 status.fit_max_abs_outlier_residual_us,
                 static_cast<unsigned long long>(status.inferred_missing_edges),
                 static_cast<unsigned long long>(status.holdover_entries),
                 static_cast<unsigned>(status.isr_queue_drops),
                 status.rtc_temperature_valid ? 1u : 0u,
                 status.rtc_temperature_valid
                     ? static_cast<double>(status.rtc_temperature_c)
                     : 0.0,
                 static_cast<int>(status.sqw_isr_core_id),
                 static_cast<unsigned>(status.sqw_interval_samples),
                 static_cast<long long>(status.sqw_last_interval_us),
                 status.sqw_interval_mean_us,
                 status.sqw_interval_rms_jitter_us,
                 status.sqw_interval_p2p_us,
                 static_cast<unsigned>(status.sqw_trace_samples),
                 static_cast<unsigned>(status.sqw_trace_overwrites));
    }
}
#endif

void WifiEventHandler(void *, esp_event_base_t event_base, int32_t event_id,
                      void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(kTag, "Wi-Fi station started; connecting to SSID '%s'",
                 CONFIG_FACTORY_WIFI_SSID);
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
        xEventGroupClearBits(wifi_event_group, kWifiConnectedBit);
        clock_sync.valid.store(false, std::memory_order_release);
        ESP_LOGW(kTag,
                 "Wi-Fi disconnected: reason=%u; clock synchronization invalidated; reconnecting",
                 static_cast<unsigned>(event->reason));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<ip_event_got_ip_t *>(event_data);

        if (!IsAcceptedIpInfo(event->ip_info)) {
            xEventGroupClearBits(wifi_event_group, kWifiConnectedBit);
            clock_sync.valid.store(false, std::memory_order_release);
            const uint32_t attempt = rejected_network_count.fetch_add(1) + 1;
            ESP_LOGW(kTag,
                     "Rejected DHCP lease: ip=" IPSTR " mask=" IPSTR " gw=" IPSTR
                     "; expected 192.168.0.0/24 with gateway 192.168.0.1 (attempt=%u)",
                     IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
                     IP2STR(&event->ip_info.gw), static_cast<unsigned>(attempt));
            ESP_LOGW(kTag,
                     "UDP command service remains disabled; requesting a fresh DHCP lease");
            RequestDhcpRetry();
            return;
        }

        rejected_network_count.store(0);
        ESP_LOGI(kTag, "Accepted factory LAN: ip=" IPSTR " mask=" IPSTR " gw=" IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
                 IP2STR(&event->ip_info.gw));
        xEventGroupSetBits(wifi_event_group, kWifiConnectedBit);
    }
}

void InitialiseNvs() {
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

void InitialiseWifi() {
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_event_group == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(wifi_sta_netif == nullptr ? ESP_FAIL : ESP_OK);
    // Keep DHCP addressing, but derive the station hostname from the unique
    // factory device ID so identity-only fleet profiles cannot clone ESP01's
    // network hostname.
    ESP_ERROR_CHECK(esp_netif_set_hostname(wifi_sta_netif, CONFIG_FACTORY_DEVICE_ID));
    ESP_LOGI(kTag, "DHCP station hostname: %s", CONFIG_FACTORY_DEVICE_ID);

    if (xTaskCreate(&DhcpRetryTask, "dhcp_retry", 3072, nullptr, 4,
                    &dhcp_retry_task_handle) != pdPASS) {
        ESP_LOGE(kTag, "Could not create DHCP retry task");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &WifiEventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &WifiEventHandler, nullptr));

    wifi_config_t wifi_config{};
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid),
                 CONFIG_FACTORY_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(wifi_config.sta.password),
                 CONFIG_FACTORY_WIFI_PASSWORD,
                 sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode =
        std::strlen(CONFIG_FACTORY_WIFI_PASSWORD) == 0 ? WIFI_AUTH_OPEN
                                                       : WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(kTag, "Wi-Fi power saving disabled for lower timing jitter");
}

bool SendPacket(int socket_fd, const sockaddr_in &peer, const char *packet,
                int length, const char *description,
                int64_t *sendto_entry_us = nullptr,
                int64_t *sendto_return_us = nullptr) {
    if (length <= 0 ||
        static_cast<std::size_t>(length) > factory_timer::kMaxPacketLength) {
        ESP_LOGE(kTag, "Could not format %s packet", description);
        return false;
    }
    const int64_t entry_us = esp_timer_get_time();
    if (sendto_entry_us != nullptr) *sendto_entry_us = entry_us;
    const int sent = sendto(socket_fd, packet, length, 0,
                            reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
    const int64_t return_us = esp_timer_get_time();
    if (sendto_return_us != nullptr) *sendto_return_us = return_us;
    if (sent != length) {
        ESP_LOGW(kTag, "%s transmission failed: errno=%d", description, errno);
        return false;
    }
#if defined(CONFIG_FACTORY_DEBUG_LOGS) && CONFIG_FACTORY_DEBUG_LOGS
    char peer_address[INET_ADDRSTRLEN]{};
    inet_ntoa_r(peer.sin_addr, peer_address, sizeof(peer_address));
#endif
    FACTORY_DEBUG_LOGI(kTag, "%s transmitted to %s:%u: %.*s", description,
                       peer_address, static_cast<unsigned>(ntohs(peer.sin_port)),
                       length, packet);
    return true;
}

void SendAck(int socket_fd, const sockaddr_in &peer,
             const factory_timer::CommandPacket &command,
             factory_timer::AckResult result) {
    char packet[factory_timer::kMaxPacketLength + 1]{};
    const int length = factory_timer::FormatAck(
        packet, sizeof(packet), CONFIG_FACTORY_DEVICE_ID, command, result);
    SendPacket(socket_fd, peer, packet, length, "ACK");
}

struct WifiDiagnostics {
    int rssi_dbm{-127};
    uint8_t channel{};
    char bssid[18]{"00:00:00:00:00:00"};
};

WifiDiagnostics ReadWifiDiagnostics() {
    WifiDiagnostics diagnostics{};
    wifi_ap_record_t ap{};
    const esp_err_t ap_result = esp_wifi_sta_get_ap_info(&ap);
    if (ap_result != ESP_OK) {
        return diagnostics;
    }

    diagnostics.rssi_dbm = static_cast<int>(ap.rssi);
    diagnostics.channel = ap.primary;
    std::snprintf(
        diagnostics.bssid, sizeof(diagnostics.bssid),
        "%02X:%02X:%02X:%02X:%02X:%02X",
        static_cast<unsigned>(ap.bssid[0]),
        static_cast<unsigned>(ap.bssid[1]),
        static_cast<unsigned>(ap.bssid[2]),
        static_cast<unsigned>(ap.bssid[3]),
        static_cast<unsigned>(ap.bssid[4]),
        static_cast<unsigned>(ap.bssid[5]));
    return diagnostics;
}

constexpr uint16_t kRtcStartMinimumFitPoints = 129;
constexpr double kRtcStartMaximumFitRmsUs = 3.0;

struct RtcStatusFields {
    const char *state = "DISABLED";
    uint16_t fit_points = 0;
    double fit_rms_us = 0.0;
    uint32_t queue_drops = 0;
    bool temperature_valid = false;
    double rate_ppm_vs_rtc = 0.0;
    uint64_t fit_outliers = 0;
    uint64_t accepted_edges = 0;
    uint64_t inferred_missing_edges = 0;
    uint64_t holdover_entries = 0;
    double temperature_c = 0.0;
    int sqw_core = -1;
};

RtcStatusFields CurrentRtcStatusFields() {
    RtcStatusFields fields{};
#if defined(CONFIG_FACTORY_DS3231_DISCIPLINE) && CONFIG_FACTORY_DS3231_DISCIPLINE
    if (!rtc_discipline_ready) {
        fields.state = "UNINITIALIZED";
        return fields;
    }
    rtc_discipline_status_t status{};
    rtc_discipline_get_status(&status);
    fields.state = rtc_discipline_state_name(status.state);
    fields.fit_points = status.fit_point_count;
    fields.fit_rms_us = status.fit_rms_us;
    fields.queue_drops = status.isr_queue_drops;
    fields.temperature_valid = status.rtc_temperature_valid;
    fields.rate_ppm_vs_rtc = status.local_rate_ppm_vs_rtc;
    fields.fit_outliers = status.fit_outlier_edges;
    fields.accepted_edges = status.accepted_edges;
    fields.inferred_missing_edges = status.inferred_missing_edges;
    fields.holdover_entries = status.holdover_entries;
    fields.temperature_c = status.rtc_temperature_valid
        ? static_cast<double>(status.rtc_temperature_c)
        : 0.0;
    fields.sqw_core = status.sqw_isr_core_id;
#endif
    return fields;
}

const char *CurrentRtcDisciplineStateName() {
    return CurrentRtcStatusFields().state;
}

bool RtcRunGateQualified(rtc_discipline_status_t *out_status = nullptr) {
#if defined(CONFIG_FACTORY_DS3231_DISCIPLINE) && CONFIG_FACTORY_DS3231_DISCIPLINE
    if (!rtc_discipline_ready) return false;
    rtc_discipline_status_t status{};
    rtc_discipline_get_status(&status);
    if (out_status != nullptr) *out_status = status;
    return status.state == RTC_DISCIPLINE_LOCKED &&
           status.fit_point_count >= kRtcStartMinimumFitPoints &&
           std::isfinite(status.fit_rms_us) &&
           status.fit_rms_us <= kRtcStartMaximumFitRmsUs &&
           status.isr_queue_drops == 0 &&
           status.rtc_temperature_valid;
#else
    (void)out_status;
    return true;
#endif
}

void SendStatus(int socket_fd, const sockaddr_in &peer,
                const factory_timer::TimerSnapshot &snapshot,
                StatusSendTiming *timing = nullptr) {
    if (timing != nullptr) timing->send_status_begin_us = esp_timer_get_time();
    if (timing != nullptr) timing->wifi_diag_begin_us = esp_timer_get_time();
    const WifiDiagnostics diagnostics = ReadWifiDiagnostics();
    if (timing != nullptr) timing->wifi_diag_end_us = esp_timer_get_time();
    if (timing != nullptr) timing->rtc_state_begin_us = esp_timer_get_time();
    const RtcStatusFields rtc = CurrentRtcStatusFields();
    if (timing != nullptr) timing->rtc_state_end_us = esp_timer_get_time();
    uint8_t health_flags = 0;
    int64_t sync_source_offset_us = 0;
    int64_t sync_epoch_local_us = 0;
    int64_t sync_epoch_disciplined_us = 0;
    int64_t sync_master_minus_disciplined_us = 0;
    if (clock_sync.valid.load(std::memory_order_acquire) &&
        clock_sync.offset_epoch_local_us > 0 &&
        clock_sync.offset_epoch_disciplined_us > 0) {
        health_flags |= 0x01u;
        sync_source_offset_us = clock_sync.source_offset_us;
        sync_epoch_local_us = clock_sync.offset_epoch_local_us;
        sync_epoch_disciplined_us = clock_sync.offset_epoch_disciplined_us;
        sync_master_minus_disciplined_us =
            sync_source_offset_us + sync_epoch_local_us - sync_epoch_disciplined_us;
    }

    int64_t start_error_us = 0;
    int64_t scheduler_lateness_us = 0;
    if (last_start_health.valid &&
        last_start_health.command_id == snapshot.last_command_id) {
        health_flags |= 0x02u;
        start_error_us = last_start_health.start_error_us;
        scheduler_lateness_us = last_start_health.scheduler_lateness_us;
    }

    factory_display_health_t display_health{};
#if defined(CONFIG_FACTORY_DISPLAY_TEST) && CONFIG_FACTORY_DISPLAY_TEST
    factory_display_get_health(&display_health);
#endif
    int64_t start_publish_lateness_us = 0;
    int64_t worst_publish_lateness_us = 0;
    uint32_t frame_not_ready_count = 0;
    if (display_health.valid && display_health.command_id == snapshot.last_command_id) {
        health_flags |= 0x04u;
        start_publish_lateness_us = display_health.start_publish_lateness_us;
        worst_publish_lateness_us = display_health.worst_publish_lateness_us;
        frame_not_ready_count = display_health.frame_not_ready_count;
    }

    const uint64_t cpu0_overflow_total64 =
        static_cast<uint64_t>(display_health.cpu0_monitor_overflow) +
        static_cast<uint64_t>(display_health.cpu0_commit_overflow);
    const uint32_t cpu0_overflow_total = cpu0_overflow_total64 > UINT32_MAX
        ? UINT32_MAX
        : static_cast<uint32_t>(cpu0_overflow_total64);
    const bool cpu0_interrupt_level_match =
        display_health.cpu0_sampler_intr_level != 0 &&
        display_health.cpu0_sampler_intr_level == display_health.cpu0_commit_intr_level;

    char packet[factory_timer::kMaxPacketLength + 1]{};
    const int length = factory_timer::FormatStatus(
        packet, sizeof(packet), CONFIG_FACTORY_DEVICE_ID,
        snapshot.last_command_id, snapshot.state, snapshot.remaining_seconds,
        diagnostics.rssi_dbm, diagnostics.channel, diagnostics.bssid,
        rtc.state, rtc.fit_points, rtc.fit_rms_us, rtc.queue_drops,
        rtc.temperature_valid, rtc.rate_ppm_vs_rtc, rtc.fit_outliers,
        rtc.temperature_c, rtc.sqw_core, health_flags,
        sync_source_offset_us, sync_epoch_local_us, sync_epoch_disciplined_us,
        sync_master_minus_disciplined_us, start_error_us, scheduler_lateness_us,
        start_publish_lateness_us, worst_publish_lateness_us,
        frame_not_ready_count, rtc.accepted_edges, rtc.inferred_missing_edges,
        rtc.holdover_entries,
        display_health.cpu0_monitor_valid,
        display_health.cpu0_monitor_samples,
        display_health.cpu0_monitor_event_count,
        display_health.cpu0_monitor_worst_us,
        display_health.cpu0_monitor_worst_task[0] != '\0'
            ? std::string_view(display_health.cpu0_monitor_worst_task)
            : std::string_view("NONE"),
        display_health.cpu0_commit_late_count,
        display_health.cpu0_commit_worst_us,
        display_health.cpu0_commit_overlap,
        display_health.cpu0_wrong_core_callbacks,
        cpu0_overflow_total,
        cpu0_interrupt_level_match,
        display_health.cpu0_monitor_missed_periods,
        FirmwareElfSha8());
    if (timing != nullptr) timing->format_done_us = esp_timer_get_time();
    SendPacket(socket_fd, peer, packet, length, "STATUS",
               timing != nullptr ? &timing->sendto_entry_us : nullptr,
               timing != nullptr ? &timing->sendto_return_us : nullptr);
    if (timing != nullptr) timing->send_status_end_us = esp_timer_get_time();
}

#if defined(CONFIG_FACTORY_START_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_START_EDGE_DIAGNOSTICS
void SetStartDiagnosticEdge(bool high) {
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_FACTORY_START_EDGE_GPIO), high ? 1 : 0);
}
#else
void SetStartDiagnosticEdge(bool) {}
#endif

uint32_t WaitExperimentalReplyDelayUs(uint32_t requested_delay_us) {
    if (requested_delay_us == 0) return 0;

    const int64_t start_us = esp_timer_get_time();
    if (requested_delay_us > static_cast<uint32_t>(kExperimentalDelayFineLeadUs + kFreeRtosTickUs)) {
        // Sleep most of a long diagnostic delay so the command task does not
        // busy-spin for hundreds of milliseconds. Stop at least ~2 ms early,
        // then use esp_timer_get_time() for the precision tail. Any scheduling
        // overshoot is measured and reported to the controller.
        const int64_t coarse_budget_us =
            static_cast<int64_t>(requested_delay_us) - kExperimentalDelayFineLeadUs;
        const TickType_t coarse_ticks = static_cast<TickType_t>(coarse_budget_us / kFreeRtosTickUs);
        if (coarse_ticks > 0) {
            vTaskDelay(coarse_ticks);
        }
    }

    while ((esp_timer_get_time() - start_us) < requested_delay_us) {
        // Experimental microsecond reverse-path delay. CommandTask priority is
        // intentionally lower than Wi-Fi/LWIP system work, so occasional
        // preemption appears as positive overshoot and is logged, not hidden.
    }

    const int64_t actual_us = esp_timer_get_time() - start_us;
    if (actual_us <= 0) return 0;
    if (actual_us > UINT32_MAX) return UINT32_MAX;
    return static_cast<uint32_t>(actual_us);
}

void InitialiseDieTemperatureSensor() {
#if CONFIG_IDF_TARGET_ESP32S3
    // BG-1 uses die temperature only as a covariate for clock-rate analysis.
    // The timer remains fully functional if TSENS installation is unavailable.
    temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    const esp_err_t install_result =
        temperature_sensor_install(&config, &die_temperature_sensor);
    if (install_result != ESP_OK) {
        ESP_LOGW(kTag, "Die-temperature sensor unavailable: install failed (%s)",
                 esp_err_to_name(install_result));
        die_temperature_sensor = nullptr;
        return;
    }

    const esp_err_t enable_result = temperature_sensor_enable(die_temperature_sensor);
    if (enable_result != ESP_OK) {
        ESP_LOGW(kTag, "Die-temperature sensor unavailable: enable failed (%s)",
                 esp_err_to_name(enable_result));
        temperature_sensor_uninstall(die_temperature_sensor);
        die_temperature_sensor = nullptr;
        return;
    }

    die_temperature_sensor_ready = true;
    ESP_LOGI(kTag, "BG-1 die-temperature telemetry enabled");
#else
    ESP_LOGI(kTag, "BG-1 die-temperature telemetry unavailable on this target");
#endif
}

bool ReadDieTemperatureMilliCelsius(int32_t &temperature_milli_c) {
#if CONFIG_IDF_TARGET_ESP32S3
    if (!die_temperature_sensor_ready || die_temperature_sensor == nullptr) {
        return false;
    }

    float celsius = 0.0f;
    if (temperature_sensor_get_celsius(die_temperature_sensor, &celsius) != ESP_OK ||
        !std::isfinite(celsius)) {
        return false;
    }

    temperature_milli_c = static_cast<int32_t>(std::lround(celsius * 1000.0f));
    return true;
#else
    (void)temperature_milli_c;
    return false;
#endif
}

void NoteSyncReplySession(uint64_t sync_id) {
    // v6.15 deliberately retains all foreground/shadow/verification samples
    // instead of resetting the trace every time the controller changes sync_id.
    sync_reply_trace_sync_id = sync_id;
}

void QueueSyncReplyTrace(const SyncReplyTraceRecord &record) {
    if (sync_reply_trace_count >= kSyncReplyTraceCapacity) {
        sync_reply_trace_overflow = true;
        return;
    }
    sync_reply_trace[sync_reply_trace_count++] = record;
}

void DiagnosticDumpPace() {
#if defined(CONFIG_FACTORY_POST_RUN_TIMING_DUMP) && CONFIG_FACTORY_POST_RUN_TIMING_DUMP
    vTaskDelay(1);
#endif
}

void DumpSyncReplyTrace() {
    if (sync_reply_trace_count == 0) return;
    ESP_LOGI(kTag,
             "SYNC_REPLY_TRACE_BEGIN sync=%016llX count=%u capacity=%u overflow=%u",
             static_cast<unsigned long long>(sync_reply_trace_sync_id),
             static_cast<unsigned>(sync_reply_trace_count),
             static_cast<unsigned>(kSyncReplyTraceCapacity),
             sync_reply_trace_overflow ? 1u : 0u);
    for (size_t i = 0; i < sync_reply_trace_count; ++i) {
        const SyncReplyTraceRecord &r = sync_reply_trace[i];
        ESP_LOGI(kTag,
                 "SYNC_REPLY_TRACE sample=%u sync=%016llX selected=%u t3_late_patch=%u t1_master_us=%lld ip_input_us=%lld ip_input_to_t2_us=%lld t2_local_us=%lld t3_local_us=%lld t3_minus_t2_us=%lld format_done_us=%lld t3_to_sendto_entry_us=%lld sendto_entry_us=%lld sendto_return_us=%lld sendto_duration_us=%lld t3_to_sendto_return_us=%lld requested_delay_us=%u reported_hold_us=%u bytes=%u sent=%u",
                 static_cast<unsigned>(i + 1),
                 static_cast<unsigned long long>(r.sync_id),
                 r.selected_by_sync_set ? 1u : 0u,
                 r.t3_late_patch ? 1u : 0u,
                 static_cast<long long>(r.master_t1_us),
                 static_cast<long long>(r.ip_input_us),
                 static_cast<long long>(r.ip_input_us >= 0 ? r.local_t2_us - r.ip_input_us : -1),
                 static_cast<long long>(r.local_t2_us),
                 static_cast<long long>(r.local_t3_us),
                 static_cast<long long>(r.local_t3_us - r.local_t2_us),
                 static_cast<long long>(r.format_done_us),
                 static_cast<long long>(r.sendto_entry_us - r.local_t3_us),
                 static_cast<long long>(r.sendto_entry_us),
                 static_cast<long long>(r.sendto_return_us),
                 static_cast<long long>(r.sendto_return_us - r.sendto_entry_us),
                 static_cast<long long>(r.sendto_return_us - r.local_t3_us),
                 static_cast<unsigned>(r.requested_delay_us),
                 static_cast<unsigned>(r.reported_hold_us),
                 static_cast<unsigned>(r.packet_length),
                 r.sent ? 1u : 0u);
        DiagnosticDumpPace();
    }
    ESP_LOGI(kTag, "SYNC_REPLY_TRACE_END");
    sync_reply_trace_count = 0;
    sync_reply_trace_overflow = false;
    sync_reply_trace_sync_id = 0;
}

void SendSyncReply(int socket_fd, const sockaddr_in &peer,
                   const factory_timer::SyncRequestPacket &request,
                   int64_t ip_input_us, int64_t local_t2_us) {
    NoteSyncReplySession(request.sync_id);
    int32_t die_temperature_milli_c = 0;
    const bool have_die_temperature =
        request.request_die_temperature &&
        ReadDieTemperatureMilliCelsius(die_temperature_milli_c);

    char packet[factory_timer::kMaxPacketLength + 1]{};
    int length = -1;
    int64_t local_t3_us = 0;
    int64_t format_done_us = 0;
    uint32_t reported_hold_us = 0;
    bool t3_late_patch = false;

    if (request.artificial_reply_delay_us == 0) {
        // Pre-build the ordinary reply with a fixed-width T3 placeholder. T3 is
        // then sampled and patched immediately before sendto(), removing the
        // ~0.3 ms packet-formatting interval from the reverse-leg timestamp.
        // Leading decimal zeroes preserve the existing numeric protocol.
        const int64_t ingress_local_us = ip_input_us > 0 ? ip_input_us : 0;
        if (have_die_temperature) {
            length = std::snprintf(packet, sizeof(packet),
                                   "FCT2|SYNC_REPLY|%s|%016llX|%lld|%lld|00000000000000000000|0|%lld|%ld",
                                   CONFIG_FACTORY_DEVICE_ID,
                                   static_cast<unsigned long long>(request.sync_id),
                                   static_cast<long long>(request.master_t1_us),
                                   static_cast<long long>(local_t2_us),
                                   static_cast<long long>(ingress_local_us),
                                   static_cast<long>(die_temperature_milli_c));
        } else {
            length = std::snprintf(packet, sizeof(packet),
                                   "FCT2|SYNC_REPLY|%s|%016llX|%lld|%lld|00000000000000000000|0|%lld",
                                   CONFIG_FACTORY_DEVICE_ID,
                                   static_cast<unsigned long long>(request.sync_id),
                                   static_cast<long long>(request.master_t1_us),
                                   static_cast<long long>(local_t2_us),
                                   static_cast<long long>(ingress_local_us));
        }
        format_done_us = esp_timer_get_time();
        char *placeholder = std::strstr(packet, "|00000000000000000000");
        local_t3_us = esp_timer_get_time();
        if (placeholder != nullptr) {
            char t3_fixed[21]{};
            const int n = std::snprintf(t3_fixed, sizeof(t3_fixed), "%020lld",
                                        static_cast<long long>(local_t3_us));
            if (n == 20) {
                std::memcpy(placeholder + 1, t3_fixed, 20);
                t3_late_patch = true;
            }
        }
    } else {
        // Positive-control path keeps the established semantics: T3 precedes
        // the intentional hold so the injected reverse delay remains visible.
        local_t3_us = esp_timer_get_time();
        const uint32_t busy_wait_us =
            WaitExperimentalReplyDelayUs(request.artificial_reply_delay_us);
        length = factory_timer::FormatSyncReply(
            packet, sizeof(packet), CONFIG_FACTORY_DEVICE_ID, request.sync_id,
            request.master_t1_us, local_t2_us, local_t3_us,
            ip_input_us > 0 ? ip_input_us : 0, busy_wait_us,
            have_die_temperature, die_temperature_milli_c);
        const int64_t held_us_64 = esp_timer_get_time() - local_t3_us;
        if (held_us_64 <= 0) reported_hold_us = 0;
        else if (held_us_64 > UINT32_MAX) reported_hold_us = UINT32_MAX;
        else reported_hold_us = static_cast<uint32_t>(held_us_64);
        length = factory_timer::FormatSyncReply(
            packet, sizeof(packet), CONFIG_FACTORY_DEVICE_ID, request.sync_id,
            request.master_t1_us, local_t2_us, local_t3_us,
            ip_input_us > 0 ? ip_input_us : 0, reported_hold_us,
            have_die_temperature, die_temperature_milli_c);
        format_done_us = esp_timer_get_time();
    }

    int64_t sendto_entry_us = 0;
    int64_t sendto_return_us = 0;
    const bool sent = SendPacket(socket_fd, peer, packet, length, "SYNC_REPLY",
                                 &sendto_entry_us, &sendto_return_us);

    SyncReplyTraceRecord sync_trace{};
    sync_trace.sync_id = request.sync_id;
    sync_trace.master_t1_us = request.master_t1_us;
    sync_trace.ip_input_us = ip_input_us;
    sync_trace.local_t2_us = local_t2_us;
    sync_trace.local_t3_us = local_t3_us;
    sync_trace.format_done_us = format_done_us;
    sync_trace.sendto_entry_us = sendto_entry_us;
    sync_trace.sendto_return_us = sendto_return_us;
    sync_trace.requested_delay_us = request.artificial_reply_delay_us;
    sync_trace.reported_hold_us = reported_hold_us;
    sync_trace.packet_length = length > 0 ? static_cast<uint16_t>(length) : 0U;
    sync_trace.sent = sent;
    sync_trace.t3_late_patch = t3_late_patch;
    QueueSyncReplyTrace(sync_trace);
}

void SendSyncApplied(int socket_fd, const sockaddr_in &peer,
                     const factory_timer::SyncSetPacket &sync_set) {
    char packet[factory_timer::kMaxPacketLength + 1]{};
    const int length = factory_timer::FormatSyncApplied(
        packet, sizeof(packet), CONFIG_FACTORY_DEVICE_ID, sync_set.sync_id,
        sync_set.master_minus_local_offset_us, sync_set.best_rtt_us,
        sync_set.offset_epoch_local_us);
    SendPacket(socket_fd, peer, packet, length, "SYNC_APPLIED");
}

void SendStarted(int socket_fd, const sockaddr_in &peer,
                 const factory_timer::TimerSnapshot &snapshot,
                 int64_t local_start_us, int64_t estimated_master_start_us,
                 int64_t target_master_start_us) {
    char packet[factory_timer::kMaxPacketLength + 1]{};
    const int length = factory_timer::FormatStarted(
        packet, sizeof(packet), CONFIG_FACTORY_DEVICE_ID, snapshot.last_command_id,
        local_start_us, estimated_master_start_us, target_master_start_us);
    SendPacket(socket_fd, peer, packet, length, "STARTED");
}


void StopStartTimerLocked() {
    // Wake the deadline task so it re-reads the countdown state. If the current
    // command was reset/cancelled, it will observe that it is no longer ARMED.
    if (timer_task_handle != nullptr) {
        xTaskNotifyGive(timer_task_handle);
    }
}

void ArmStartTimerLocked(int64_t local_start_us) {
    // Ensure every armed command produces a fresh rising edge even if a prior
    // diagnostic trial did not complete its normal RESET cleanup.
    SetStartDiagnosticEdge(false);
    const int64_t arm_local_us = esp_timer_get_time();
    const int64_t delay_us = local_start_us - arm_local_us;

    ESP_LOGI(kTag,
             "Countdown scheduler armed: source=deadline_task target_local_us=%lld arm_local_us=%lld delay_us=%lld tick_us=%lld fine_lead_us=%lld",
             static_cast<long long>(local_start_us),
             static_cast<long long>(arm_local_us),
             static_cast<long long>(delay_us),
             static_cast<long long>(kFreeRtosTickUs),
             static_cast<long long>(kSchedulerFineLeadUs));

    // The scheduler task blocks on this notification. A subsequent RESET or
    // re-arm also notifies it, so it never has to poll while waiting seconds.
    if (timer_task_handle != nullptr) {
        xTaskNotifyGive(timer_task_handle);
    }
}

void TimerTask(void *) {
    ESP_LOGI(kTag,
             "Countdown deadline task ready (priority=%u tick_us=%lld fine_lead_us=%lld)",
             static_cast<unsigned>(kTimerTaskPriority),
             static_cast<long long>(kFreeRtosTickUs),
             static_cast<long long>(kSchedulerFineLeadUs));

    while (true) {
        // Sleep indefinitely until START/START_AT, RESET, or a re-arm changes
        // the scheduler state.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (true) {
            int64_t target_local_us = 0;
            factory_timer::TimerSnapshot current{};

            xSemaphoreTake(countdown_mutex, portMAX_DELAY);
            current = countdown.Snapshot();
            if (current.state == factory_timer::TimerState::Armed) {
                target_local_us = countdown.ScheduledStartMicroseconds();
            }
            xSemaphoreGive(countdown_mutex);

            if (current.state != factory_timer::TimerState::Armed ||
                target_local_us <= 0) {
                break;
            }

            int64_t now = esp_timer_get_time();
            int64_t remaining_us = target_local_us - now;

            if (remaining_us > kSchedulerFineLeadUs) {
                // Sleep for whole RTOS ticks, deliberately stopping short of
                // the target. Because the division is floored, the remaining
                // final busy window is bounded to roughly one RTOS tick plus
                // kSchedulerFineLeadUs (about 12 ms at 100 Hz).
                const int64_t sleep_budget_us = remaining_us - kSchedulerFineLeadUs;
                const TickType_t ticks_to_wait = static_cast<TickType_t>(
                    sleep_budget_us / kFreeRtosTickUs);

                if (ticks_to_wait > 0) {
                    // A new START/RESET notification wakes us immediately so
                    // we can re-read the target rather than waiting for the old
                    // deadline.
                    ulTaskNotifyTake(pdTRUE, ticks_to_wait);
                    continue;
                }
            }

            // Final precision phase. This is intentionally short (normally
            // <= one 100-Hz RTOS tick + 2 ms), unlike the old UDP task which
            // polled continuously for the entire lifetime of the firmware.
            do {
                now = esp_timer_get_time();
            } while (now < target_local_us);

            TimerStartEvent event{};
            bool started = false;

            const int64_t disciplined_now =
                rtc_discipline_local_to_disciplined_us(now);

            xSemaphoreTake(countdown_mutex, portMAX_DELAY);
            const auto before = countdown.Snapshot();
            const auto after = countdown.Update(now, disciplined_now);

            if (before.state == factory_timer::TimerState::Armed &&
                after.state == factory_timer::TimerState::Running) {
                event.snapshot = after;
                event.actual_local_start_us = now;
                event.actual_disciplined_start_us = countdown.StartRunningMicroseconds();
                event.target_local_start_us = target_local_us;

                if (scheduled_start_metadata.valid &&
                    scheduled_start_metadata.command_id == after.last_command_id) {
                    event.peer = scheduled_start_metadata.peer;
                    event.have_peer = scheduled_start_metadata.have_peer;
                    event.target_master_start_us =
                        scheduled_start_metadata.target_master_start_us;
                    event.master_minus_local_offset_us =
                        scheduled_start_metadata.master_minus_local_offset_us;
                    event.sync_id = scheduled_start_metadata.sync_id;
                    event.best_rtt_us = scheduled_start_metadata.best_rtt_us;
                    event.sync_applied_local_us =
                        scheduled_start_metadata.sync_applied_local_us;
                    event.sync_estimated_master_us =
                        scheduled_start_metadata.sync_estimated_master_us;
                    event.source_offset_us = scheduled_start_metadata.source_offset_us;
                    event.offset_epoch_local_us = scheduled_start_metadata.offset_epoch_local_us;
                    event.offset_epoch_disciplined_us =
                        scheduled_start_metadata.offset_epoch_disciplined_us;
                    event.estimated_master_epoch_us =
                        scheduled_start_metadata.estimated_master_epoch_us;
                    if (event.target_master_start_us > 0) {
                        if (event.offset_epoch_disciplined_us > 0 &&
                            event.estimated_master_epoch_us > 0) {
                            event.estimated_master_start_us =
                                event.estimated_master_epoch_us +
                                (event.actual_disciplined_start_us -
                                 event.offset_epoch_disciplined_us);
                        } else {
                            event.estimated_master_start_us =
                                now + scheduled_start_metadata.master_minus_local_offset_us;
                        }
                    }
                }

                // Physical validation point: the output transition is emitted
                // from the same high-priority task that owns Armed -> Running.
                // gpio_set_level latency is sub-microsecond-scale relative to
                // the 100+ us effects being measured and can be calibrated.
                SetStartDiagnosticEdge(true);
                scheduled_start_metadata.valid = false;
                started = true;
            }
            xSemaphoreGive(countdown_mutex);

#if defined(CONFIG_FACTORY_DISPLAY_TEST) && CONFIG_FACTORY_DISPLAY_TEST
            if (started) {
                factory_display_note_started(event.snapshot.last_command_id,
                                             event.actual_disciplined_start_us);
            }
#endif
            if (started) {
                // Positive-control diagnostics are launched only after the exact
                // Armed -> Running transition and never execute NVS work here.
                flash_guard_diag_countdown_started(
                    event.snapshot.last_command_id, event.actual_local_start_us);
            }

            if (!started) {
                // State or target changed on the other core while we were in
                // the short final window. Re-read it instead of starting a
                // stale command.
                continue;
            }

            // Do not log from TimerTask at the START deadline. At 115200 baud a
            // long ESP_LOGI line can block once the UART TX FIFO fills and, on
            // classic ESP32, delay the lower-priority display flip on CPU0 by
            // milliseconds. Preserve the exact start timestamps in TimerStartEvent
            // and log them later from CommandTask.
            if (xQueueSend(timer_event_queue, &event, 0) != pdTRUE) {
                ESP_LOGW(kTag,
                         "Timer event queue full; STARTED telemetry may be missing for command=%016llX",
                         static_cast<unsigned long long>(event.snapshot.last_command_id));
            }
            break;
        }
    }
}

#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
const char *RxBoundaryPacketTypeName(const RxBoundaryEvent &event) {
    if (!event.parsed) return "RAW";
    switch (event.packet_type) {
        case factory_timer::MasterPacketType::SyncRequest: return "SYNC";
        case factory_timer::MasterPacketType::SyncSet: return "SYNC_SET";
        case factory_timer::MasterPacketType::Command:
            return factory_timer::CommandTypeName(event.command_type);
    }
    return "UNKNOWN";
}

bool ShouldSuppressRunningHeartbeat(const factory_timer::TimerSnapshot &snapshot) {
#if defined(CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT) && CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT
    return snapshot.state == factory_timer::TimerState::Running;
#else
    (void)snapshot;
    return false;
#endif
}

void RecordSuppressedRunningHeartbeat() {
    running_heartbeat_suppressed.fetch_add(1, std::memory_order_relaxed);
}

void ResetNetworkSilentDiagnostics() {
    running_heartbeat_suppressed.store(0, std::memory_order_relaxed);
}

void ResetNetworkTimingDiagnostics() {
    rx_boundary_trace_count = 0;
    rx_boundary_trace_overflow = false;
    status_path_trace_count = 0;
    status_path_trace_overflow = false;
    status_tx_trace_count = 0;
    status_tx_trace_overflow = false;
}

void InitialiseRxBoundaryTrace() {
    ESP_LOGI(kTag,
             "Control-port RX/status-path trace enabled: window_us=%lld buffered_until_run_end=1",
             static_cast<long long>(kRxBoundaryTraceWindowUs));
}

void StoreRxBoundaryEvent(const RxBoundaryEvent &event) {
    if (rx_boundary_trace_count >= kRxBoundaryTraceCapacity) {
        rx_boundary_trace_overflow = true;
        return;
    }
    rx_boundary_trace[rx_boundary_trace_count++] = event;
}

void StoreStatusPathTrace(const StatusPathTraceRecord &event) {
    if (status_path_trace_count >= kStatusPathTraceCapacity) {
        status_path_trace_overflow = true;
        return;
    }
    status_path_trace[status_path_trace_count++] = event;
}

const char *StatusSendReasonName(StatusSendReason reason) {
    switch (reason) {
        case StatusSendReason::RequestReply: return "REQUEST_REPLY";
        case StatusSendReason::TimerStarted: return "TIMER_STARTED";
        case StatusSendReason::StateChange: return "STATE_CHANGE";
        case StatusSendReason::Heartbeat: return "HEARTBEAT";
        case StatusSendReason::Other: return "OTHER";
    }
    return "OTHER";
}

void StoreStatusTxTrace(StatusSendReason reason,
                        const factory_timer::TimerSnapshot &snapshot,
                        const StatusSendTiming &timing) {
    if (snapshot.state != factory_timer::TimerState::Running) return;
    if (status_tx_trace_count >= kStatusTxTraceCapacity) {
        status_tx_trace_overflow = true;
        return;
    }
    StatusTxTraceRecord &out = status_tx_trace[status_tx_trace_count++];
    out.reason = reason;
    out.state = snapshot.state;
    out.remaining_seconds = snapshot.remaining_seconds;
    out.send = timing;
}

void SendStatusWithTrace(int socket_fd,
                         const sockaddr_in &peer,
                         const factory_timer::TimerSnapshot &snapshot,
                         StatusSendReason reason) {
    StatusSendTiming timing{};
    SendStatus(socket_fd, peer, snapshot, &timing);
    StoreStatusTxTrace(reason, snapshot, timing);
}

void FillBoundaryPosition(int64_t receive_disciplined_us,
                          int64_t start_disciplined_us,
                          uint32_t duration_seconds,
                          uint32_t *boundary_index,
                          int32_t *lead_us) {
    if (boundary_index != nullptr) *boundary_index = 0;
    if (lead_us != nullptr) *lead_us = 0;
    if (start_disciplined_us <= 0 || duration_seconds == 0) return;
    const int64_t elapsed_us = receive_disciplined_us - start_disciplined_us;
    if (elapsed_us < 0) return;
    const int64_t completed_seconds = elapsed_us / 1000000LL;
    const int64_t phase_us = elapsed_us % 1000000LL;
    const uint32_t next_boundary = static_cast<uint32_t>(completed_seconds + 1);
    if (next_boundary > duration_seconds) return;
    if (boundary_index != nullptr) *boundary_index = next_boundary;
    if (lead_us != nullptr) *lead_us = static_cast<int32_t>(1000000LL - phase_us);
}

void MaybeQueueRxNearBoundary(int64_t receive_local_us,
                              const sockaddr_in &peer,
                              const factory_timer::MasterPacket *packet) {
    factory_timer::TimerSnapshot snapshot{};
    int64_t start_disciplined_us = 0;
    xSemaphoreTake(countdown_mutex, portMAX_DELAY);
    snapshot = countdown.Snapshot();
    start_disciplined_us = countdown.StartRunningMicroseconds();
    xSemaphoreGive(countdown_mutex);

    if (snapshot.state != factory_timer::TimerState::Running ||
        start_disciplined_us <= 0 || snapshot.duration_seconds == 0) {
        return;
    }

    const int64_t receive_disciplined_us =
        rtc_discipline_local_to_disciplined_us(receive_local_us);
    uint32_t boundary_index = 0;
    int32_t lead_us = 0;
    FillBoundaryPosition(receive_disciplined_us,
                         start_disciplined_us,
                         snapshot.duration_seconds,
                         &boundary_index,
                         &lead_us);
    if (boundary_index == 0 || lead_us <= 0 || lead_us > kRxBoundaryTraceWindowUs) return;

    RxBoundaryEvent event{};
    event.receive_local_us = receive_local_us;
    event.receive_disciplined_us = receive_disciplined_us;
    event.boundary_index = boundary_index;
    event.lead_us = lead_us;
    event.source_address = peer.sin_addr.s_addr;
    event.source_port = ntohs(peer.sin_port);
    event.parsed = packet != nullptr;
    if (packet != nullptr) {
        event.packet_type = packet->type;
        if (packet->type == factory_timer::MasterPacketType::Command) {
            event.command_type = packet->command.type;
        }
    }
    StoreRxBoundaryEvent(event);
}

void DumpNetworkTimingDiagnostics() {
    ESP_LOGI(kTag,
             "RX_BOUNDARY_TRACE_BEGIN count=%u capacity=%u overflow=%u",
             static_cast<unsigned>(rx_boundary_trace_count),
             static_cast<unsigned>(kRxBoundaryTraceCapacity),
             rx_boundary_trace_overflow ? 1u : 0u);
    for (size_t i = 0; i < rx_boundary_trace_count; ++i) {
        const RxBoundaryEvent &event = rx_boundary_trace[i];
        in_addr source{};
        source.s_addr = event.source_address;
        char source_ip[INET_ADDRSTRLEN]{};
        inet_ntoa_r(source, source_ip, sizeof(source_ip));
        ESP_LOGI(kTag,
                 "RX_NEAR_BOUNDARY device=%s local_us=%lld disciplined_us=%lld boundary=%u lead_us=%ld source=%s:%u packet=%s",
                 CONFIG_FACTORY_DEVICE_ID,
                 static_cast<long long>(event.receive_local_us),
                 static_cast<long long>(event.receive_disciplined_us),
                 static_cast<unsigned>(event.boundary_index),
                 static_cast<long>(event.lead_us),
                 source_ip,
                 static_cast<unsigned>(event.source_port),
                 RxBoundaryPacketTypeName(event));
        DiagnosticDumpPace();
    }
    ESP_LOGI(kTag, "RX_BOUNDARY_TRACE_END");

    ESP_LOGI(kTag,
             "STATUS_PATH_TRACE_BEGIN count=%u capacity=%u overflow=%u",
             static_cast<unsigned>(status_path_trace_count),
             static_cast<unsigned>(kStatusPathTraceCapacity),
             status_path_trace_overflow ? 1u : 0u);
    for (size_t i = 0; i < status_path_trace_count; ++i) {
        const StatusPathTraceRecord &r = status_path_trace[i];
        in_addr source{};
        source.s_addr = r.source_address;
        char source_ip[INET_ADDRSTRLEN]{};
        inet_ntoa_r(source, source_ip, sizeof(source_ip));
        ESP_LOGI(kTag,
                 "STATUS_PATH command=%016llX source=%s:%u boundary=%u lead_us=%ld ip_input_us=%lld ip_input_to_recv_us=%lld recv_us=%lld parse_done_us=%lld rx_trace_begin_us=%lld rx_trace_end_us=%lld lock_request_us=%lld lock_acquired_us=%lld discipline_begin_us=%lld discipline_end_us=%lld process_done_us=%lld lock_release_us=%lld send_status_begin_us=%lld wifi_begin_us=%lld wifi_end_us=%lld rtc_begin_us=%lld rtc_end_us=%lld format_done_us=%lld sendto_entry_us=%lld sendto_return_us=%lld send_status_end_us=%lld handler_exit_us=%lld state=%s",
                 static_cast<unsigned long long>(r.command_id),
                 source_ip,
                 static_cast<unsigned>(r.source_port),
                 static_cast<unsigned>(r.boundary_index),
                 static_cast<long>(r.lead_us),
                 static_cast<long long>(r.ip_input_us),
                 static_cast<long long>(r.ip_input_us >= 0 ? r.recv_return_us - r.ip_input_us : -1),
                 static_cast<long long>(r.recv_return_us),
                 static_cast<long long>(r.parse_done_us),
                 static_cast<long long>(r.rx_trace_begin_us),
                 static_cast<long long>(r.rx_trace_end_us),
                 static_cast<long long>(r.lock_request_us),
                 static_cast<long long>(r.lock_acquired_us),
                 static_cast<long long>(r.discipline_begin_us),
                 static_cast<long long>(r.discipline_end_us),
                 static_cast<long long>(r.process_done_us),
                 static_cast<long long>(r.lock_release_us),
                 static_cast<long long>(r.send.send_status_begin_us),
                 static_cast<long long>(r.send.wifi_diag_begin_us),
                 static_cast<long long>(r.send.wifi_diag_end_us),
                 static_cast<long long>(r.send.rtc_state_begin_us),
                 static_cast<long long>(r.send.rtc_state_end_us),
                 static_cast<long long>(r.send.format_done_us),
                 static_cast<long long>(r.send.sendto_entry_us),
                 static_cast<long long>(r.send.sendto_return_us),
                 static_cast<long long>(r.send.send_status_end_us),
                 static_cast<long long>(r.handler_exit_us),
                 factory_timer::TimerStateName(r.snapshot_state));
        DiagnosticDumpPace();
    }
    ESP_LOGI(kTag, "STATUS_PATH_TRACE_END");

    ESP_LOGI(kTag,
             "STATUS_TX_TRACE_BEGIN count=%u capacity=%u overflow=%u",
             static_cast<unsigned>(status_tx_trace_count),
             static_cast<unsigned>(kStatusTxTraceCapacity),
             status_tx_trace_overflow ? 1u : 0u);
    for (size_t i = 0; i < status_tx_trace_count; ++i) {
        const StatusTxTraceRecord &r = status_tx_trace[i];
        ESP_LOGI(kTag,
                 "STATUS_TX reason=%s state=%s remaining=%u begin_us=%lld wifi_begin_us=%lld wifi_end_us=%lld rtc_begin_us=%lld rtc_end_us=%lld format_done_us=%lld sendto_entry_us=%lld sendto_return_us=%lld end_us=%lld",
                 StatusSendReasonName(r.reason),
                 factory_timer::TimerStateName(r.state),
                 static_cast<unsigned>(r.remaining_seconds),
                 static_cast<long long>(r.send.send_status_begin_us),
                 static_cast<long long>(r.send.wifi_diag_begin_us),
                 static_cast<long long>(r.send.wifi_diag_end_us),
                 static_cast<long long>(r.send.rtc_state_begin_us),
                 static_cast<long long>(r.send.rtc_state_end_us),
                 static_cast<long long>(r.send.format_done_us),
                 static_cast<long long>(r.send.sendto_entry_us),
                 static_cast<long long>(r.send.sendto_return_us),
                 static_cast<long long>(r.send.send_status_end_us));
        DiagnosticDumpPace();
    }
    ESP_LOGI(kTag, "STATUS_TX_TRACE_END");
}
#else
void InitialiseRxBoundaryTrace() {}
void ResetNetworkTimingDiagnostics() {}
void MaybeQueueRxNearBoundary(int64_t, const sockaddr_in &, const factory_timer::MasterPacket *) {}
void DumpNetworkTimingDiagnostics() {}
#endif

#if defined(CONFIG_FACTORY_POST_RUN_TIMING_DUMP) && CONFIG_FACTORY_POST_RUN_TIMING_DUMP
void DiagnosticDumpTask(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t begin_us = esp_timer_get_time();
        ESP_LOGI(kTag, "POST_RUN_DIAG_BEGIN local_us=%lld", static_cast<long long>(begin_us));
#if defined(CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT) && CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT
        ESP_LOGI(kTag,
                 "NETWORK_SILENT_DIAG running_heartbeat_suppressed=%u",
                 static_cast<unsigned>(running_heartbeat_suppressed.load(std::memory_order_relaxed)));
#endif
        DumpSyncReplyTrace();
        rtc_discipline_dump_sqw_trace();
        DumpNetworkTimingDiagnostics();
        flash_guard_diag_dump_run();
        const int64_t end_us = esp_timer_get_time();
        ESP_LOGI(kTag,
                 "POST_RUN_DIAG_END local_us=%lld duration_us=%lld command_task_blocked=0",
                 static_cast<long long>(end_us),
                 static_cast<long long>(end_us - begin_us));
    }
}

void InitialiseDiagnosticDumpTask() {
    if (xTaskCreatePinnedToCore(&DiagnosticDumpTask,
                                "timing_dump",
                                4096,
                                nullptr,
                                kDiagnosticDumpTaskPriority,
                                &diagnostic_dump_task_handle,
                                kControlTaskCore) != pdPASS) {
        ESP_LOGE(kTag, "Could not create asynchronous timing-dump task");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
}

void RequestPostRunDiagnosticDump() {
    if (diagnostic_dump_task_handle != nullptr) {
        xTaskNotifyGive(diagnostic_dump_task_handle);
    }
}
#else
void InitialiseDiagnosticDumpTask() {}
void RequestPostRunDiagnosticDump() {}
#endif

void InitialiseStartScheduler() {
#if defined(CONFIG_FACTORY_START_EDGE_DIAGNOSTICS) && CONFIG_FACTORY_START_EDGE_DIAGNOSTICS
    gpio_config_t diagnostic_gpio{};
    diagnostic_gpio.pin_bit_mask = 1ULL << CONFIG_FACTORY_START_EDGE_GPIO;
    diagnostic_gpio.mode = GPIO_MODE_OUTPUT;
    diagnostic_gpio.pull_up_en = GPIO_PULLUP_DISABLE;
    diagnostic_gpio.pull_down_en = GPIO_PULLDOWN_DISABLE;
    diagnostic_gpio.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&diagnostic_gpio));
    SetStartDiagnosticEdge(false);
    ESP_LOGI(kTag, "Physical START-edge diagnostic enabled on GPIO%d",
             CONFIG_FACTORY_START_EDGE_GPIO);
#endif

    countdown_mutex = xSemaphoreCreateMutex();
    if (countdown_mutex == nullptr) {
        ESP_LOGE(kTag, "Could not create countdown mutex");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    timer_event_queue = xQueueCreate(4, sizeof(TimerStartEvent));
    if (timer_event_queue == nullptr) {
        ESP_LOGE(kTag, "Could not create timer event queue");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    if (xTaskCreatePinnedToCore(&TimerTask, "countdown_timer", 4096, nullptr,
                                kTimerTaskPriority, &timer_task_handle,
                                kControlTaskCore) != pdPASS) {
        ESP_LOGE(kTag, "Could not create countdown timer task");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    ESP_LOGI(kTag,
             "High-resolution countdown scheduler ready; source=deadline_task final_spin<=~%lld us timer_task_priority=%u control_core=%d UDP receive timeout=%lld us",
             static_cast<long long>(kFreeRtosTickUs + kSchedulerFineLeadUs),
             static_cast<unsigned>(kTimerTaskPriority),
             static_cast<int>(kControlTaskCore),
             static_cast<long long>(kCommandSocketTimeoutUs));
}

int CreateCommandSocket() {
    const int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_fd < 0) return -1;

    const int reuse = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const timeval timeout{.tv_sec = 0, .tv_usec = kCommandSocketTimeoutUs};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kCommandPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        ESP_LOGE(kTag, "UDP bind on port %u failed: errno=%d", kCommandPort, errno);
        close(socket_fd);
        return -1;
    }
    ESP_LOGI(kTag, "Listening for UDP commands on port %u", kCommandPort);
    return socket_fd;
}

void LogAcceptedCommand(const factory_timer::CommandPacket &command,
                        factory_timer::AckResult result,
                        int64_t local_start_us) {
    if (result == factory_timer::AckResult::Duplicate) {
        FACTORY_DEBUG_LOGI(kTag,
                           "Duplicate command id=%016llX acknowledged without changing timer",
                           static_cast<unsigned long long>(command.command_id));
        return;
    }
    if (result == factory_timer::AckResult::Late) {
        ESP_LOGW(kTag, "START_AT rejected as late: target_master_us=%lld command=%016llX",
                 static_cast<long long>(command.start_at_master_us),
                 static_cast<unsigned long long>(command.command_id));
        return;
    }
    if (result != factory_timer::AckResult::Accepted) return;

    if (command.type == factory_timer::CommandType::StartAt) {
        // Diagnostic baseline is taken after START_AT has already been accepted
        // and scheduled. It does not participate in the deadline/ISR path.
        flash_guard_diag_begin_run(command.command_id, local_start_us);
        ESP_LOGI(kTag,
                 "Absolute countdown accepted: duration=%u target_master_us=%lld local_start_us=%lld command=%016llX",
                 static_cast<unsigned>(command.duration_seconds),
                 static_cast<long long>(command.start_at_master_us),
                 static_cast<long long>(local_start_us),
                 static_cast<unsigned long long>(command.command_id));
    } else if (command.type == factory_timer::CommandType::Start) {
        ESP_LOGI(kTag,
                 "Legacy countdown accepted: duration=%u delay_ms=%u command=%016llX",
                 static_cast<unsigned>(command.duration_seconds),
                 static_cast<unsigned>(command.start_delay_ms),
                 static_cast<unsigned long long>(command.command_id));
    } else if (command.type == factory_timer::CommandType::Reset) {
        ESP_LOGI(kTag, "Countdown reset: duration=%u command=%016llX",
                 static_cast<unsigned>(command.duration_seconds),
                 static_cast<unsigned long long>(command.command_id));
    }
}

void CommandTask(void *) {
    xEventGroupWaitBits(wifi_event_group, kWifiConnectedBit, pdFALSE, pdTRUE,
                        portMAX_DELAY);
    const int socket_fd = CreateCommandSocket();
    if (socket_fd < 0) {
        ESP_LOGE(kTag, "UDP task stopping because the socket could not be created");
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in last_peer{};
    bool have_peer = false;
    xSemaphoreTake(countdown_mutex, portMAX_DELAY);
    auto last_snapshot = countdown.Snapshot();
    xSemaphoreGive(countdown_mutex);
    int64_t last_heartbeat = 0;

    while (true) {
        if ((xEventGroupGetBits(wifi_event_group) & kWifiConnectedBit) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        char buffer[factory_timer::kMaxPacketLength + 1]{};
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        const int received = recvfrom(socket_fd, buffer, sizeof(buffer), 0,
                                      reinterpret_cast<sockaddr *>(&peer), &peer_length);
        const int64_t receive_local_us = esp_timer_get_time();
        const int64_t ip_input_us =
            received > 0 ? MatchEarlyIngress(peer, buffer, received) : -1;

        if (received > 0) {
            if (received > static_cast<int>(factory_timer::kMaxPacketLength)) {
                ESP_LOGW(kTag, "Discarded oversized UDP datagram");
            } else {
                const std::string_view text(buffer, static_cast<std::size_t>(received));
                factory_timer::MasterPacket packet{};
                factory_timer::ParseError parse_error{};
                if (!factory_timer::ParseMasterPacket(text, packet, parse_error)) {
                    MaybeQueueRxNearBoundary(receive_local_us, peer, nullptr);
                    ESP_LOGW(kTag, "Discarded invalid packet (%s)",
                             factory_timer::ParseErrorName(parse_error));
                } else {
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                    StatusPathTraceRecord status_trace{};
                    const bool trace_status_path =
                        packet.type == factory_timer::MasterPacketType::Command &&
                        packet.command.type == factory_timer::CommandType::StatusRequest;
                    if (trace_status_path) {
                        status_trace.command_id = packet.command.command_id;
                        status_trace.source_address = peer.sin_addr.s_addr;
                        status_trace.source_port = ntohs(peer.sin_port);
                        status_trace.ip_input_us = ip_input_us;
                        status_trace.recv_return_us = receive_local_us;
                        status_trace.parse_done_us = esp_timer_get_time();
                        status_trace.rx_trace_begin_us = esp_timer_get_time();
                    }
#endif
                    MaybeQueueRxNearBoundary(receive_local_us, peer, &packet);
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                    if (trace_status_path) {
                        status_trace.rx_trace_end_us = esp_timer_get_time();
                    }
#endif
                    if (packet.type == factory_timer::MasterPacketType::SyncRequest) {
                    SendSyncReply(socket_fd, peer, packet.sync_request, ip_input_us, receive_local_us);
                    FACTORY_DEBUG_LOGI(kTag,
                                       "SYNC id=%016llX t1=%lld t2=%lld artificial_reply_delay_us=%u",
                                       static_cast<unsigned long long>(packet.sync_request.sync_id),
                                       static_cast<long long>(packet.sync_request.master_t1_us),
                                       static_cast<long long>(receive_local_us),
                                       static_cast<unsigned>(packet.sync_request.artificial_reply_delay_us));
                } else if (packet.type == factory_timer::MasterPacketType::SyncSet) {
                    for (size_t i = 0; i < sync_reply_trace_count; ++i) {
                        if (sync_reply_trace[i].sync_id == packet.sync_set.sync_id) {
                            sync_reply_trace[i].selected_by_sync_set = true;
                        }
                    }
                    clock_sync.source_offset_us =
                        packet.sync_set.master_minus_local_offset_us;
                    clock_sync.best_rtt_us = packet.sync_set.best_rtt_us;
                    clock_sync.sync_id = packet.sync_set.sync_id;
                    clock_sync.applied_local_us = receive_local_us;
                    if (packet.sync_set.offset_epoch_local_us > 0) {
                        clock_sync.offset_epoch_local_us =
                            packet.sync_set.offset_epoch_local_us;
                        clock_sync.offset_epoch_disciplined_us =
                            rtc_discipline_local_to_disciplined_us(
                                clock_sync.offset_epoch_local_us);
                        clock_sync.estimated_master_epoch_us =
                            clock_sync.offset_epoch_local_us + clock_sync.source_offset_us;
                        const int64_t apply_disciplined_us =
                            rtc_discipline_local_to_disciplined_us(receive_local_us);
                        clock_sync.estimated_master_apply_us =
                            clock_sync.estimated_master_epoch_us +
                            (apply_disciplined_us - clock_sync.offset_epoch_disciplined_us);
                        clock_sync.master_minus_local_offset_us =
                            clock_sync.estimated_master_apply_us - receive_local_us;
                    } else {
                        // Preserve legacy four-timestamp benchmark/manual behavior.
                        clock_sync.offset_epoch_local_us = 0;
                        clock_sync.offset_epoch_disciplined_us = 0;
                        clock_sync.estimated_master_epoch_us = 0;
                        clock_sync.master_minus_local_offset_us =
                            clock_sync.source_offset_us;
                        clock_sync.estimated_master_apply_us =
                            receive_local_us + clock_sync.master_minus_local_offset_us;
                    }
                    clock_sync.valid.store(true, std::memory_order_release);
                    SendSyncApplied(socket_fd, peer, packet.sync_set);
                    ESP_LOGI(kTag,
                             "Clock synchronized: offset_master_minus_local_us=%lld best_rtt_us=%llu sync=%016llX source_offset_us=%lld offset_epoch_local_us=%lld offset_epoch_disciplined_us=%lld offset_master_minus_disciplined_us=%lld",
                             static_cast<long long>(clock_sync.master_minus_local_offset_us),
                             static_cast<unsigned long long>(clock_sync.best_rtt_us),
                             static_cast<unsigned long long>(clock_sync.sync_id),
                             static_cast<long long>(clock_sync.source_offset_us),
                             static_cast<long long>(clock_sync.offset_epoch_local_us),
                             static_cast<long long>(clock_sync.offset_epoch_disciplined_us),
                             static_cast<long long>(
                                 clock_sync.source_offset_us +
                                 clock_sync.offset_epoch_local_us -
                                 clock_sync.offset_epoch_disciplined_us));
#if defined(CONFIG_FACTORY_RTC_QUAL_LOGS) && CONFIG_FACTORY_RTC_QUAL_LOGS
                    rtc_discipline_status_t rtc_status{};
                    rtc_discipline_get_status(&rtc_status);
                    ESP_LOGI(kTag,
                             "RTC_SYNC_QUAL device=%s sync=%016llX local_us=%lld estimated_master_us=%lld offset_master_minus_local_us=%lld best_rtt_us=%llu state=%s rate_ppm=%+.6f rms_us=%.3f points=%u temp_valid=%u temp_c=%.2f source_offset_us=%lld offset_epoch_local_us=%lld offset_epoch_disciplined_us=%lld offset_master_minus_disciplined_us=%lld",
                             CONFIG_FACTORY_DEVICE_ID,
                             static_cast<unsigned long long>(clock_sync.sync_id),
                             static_cast<long long>(clock_sync.applied_local_us),
                             static_cast<long long>(clock_sync.estimated_master_apply_us),
                             static_cast<long long>(clock_sync.master_minus_local_offset_us),
                             static_cast<unsigned long long>(clock_sync.best_rtt_us),
                             rtc_discipline_state_name(rtc_status.state),
                             rtc_status.local_rate_ppm_vs_rtc,
                             rtc_status.fit_rms_us,
                             static_cast<unsigned>(rtc_status.fit_point_count),
                             rtc_status.rtc_temperature_valid ? 1u : 0u,
                             rtc_status.rtc_temperature_valid
                                 ? static_cast<double>(rtc_status.rtc_temperature_c)
                                 : 0.0,
                             static_cast<long long>(clock_sync.source_offset_us),
                             static_cast<long long>(clock_sync.offset_epoch_local_us),
                             static_cast<long long>(clock_sync.offset_epoch_disciplined_us),
                             static_cast<long long>(
                                 clock_sync.source_offset_us +
                                 clock_sync.offset_epoch_local_us -
                                 clock_sync.offset_epoch_disciplined_us));
#endif
                } else {
                    const auto &command = packet.command;
                    last_peer = peer;
                    have_peer = true;

                    if (command.type == factory_timer::CommandType::Brightness) {
#if defined(CONFIG_FACTORY_DISPLAY_TEST) && CONFIG_FACTORY_DISPLAY_TEST
                        factory_display_set_brightness_percent(command.brightness_percent);
#endif
                        ESP_LOGI(kTag, "Panel brightness command accepted: brightness=%u%% command=%016llX",
                                 static_cast<unsigned>(command.brightness_percent),
                                 static_cast<unsigned long long>(command.command_id));
                        SendAck(socket_fd, peer, command, factory_timer::AckResult::Accepted);
                        xSemaphoreTake(countdown_mutex, portMAX_DELAY);
                        const auto snapshot = countdown.Snapshot();
                        xSemaphoreGive(countdown_mutex);
                        SendStatus(socket_fd, peer, snapshot);
                        last_snapshot = snapshot;
                        last_heartbeat = receive_local_us;
                    } else if ((command.type == factory_timer::CommandType::StartAt ||
                                command.type == factory_timer::CommandType::Start) &&
                               !RtcRunGateQualified()) {
                        const RtcStatusFields rtc = CurrentRtcStatusFields();
                        ESP_LOGW(kTag,
                                 "%s rejected because RTC is not START-qualified "
                                 "(state=%s points=%u/%u rms_us=%.3f/%.3f queue_drops=%u temp_valid=%u): command=%016llX",
                                 factory_timer::CommandTypeName(command.type),
                                 rtc.state,
                                 static_cast<unsigned>(rtc.fit_points),
                                 static_cast<unsigned>(kRtcStartMinimumFitPoints),
                                 rtc.fit_rms_us,
                                 kRtcStartMaximumFitRmsUs,
                                 static_cast<unsigned>(rtc.queue_drops),
                                 rtc.temperature_valid ? 1u : 0u,
                                 static_cast<unsigned long long>(command.command_id));
                        SendAck(socket_fd, peer, command, factory_timer::AckResult::NotSynced);
                        xSemaphoreTake(countdown_mutex, portMAX_DELAY);
                        const auto snapshot = countdown.Snapshot();
                        xSemaphoreGive(countdown_mutex);
                        SendStatus(socket_fd, peer, snapshot);
                    } else if (command.type == factory_timer::CommandType::StartAt &&
                        !clock_sync.valid.load(std::memory_order_acquire)) {
                        ESP_LOGW(kTag,
                                 "START_AT rejected because device clock is not synchronized: command=%016llX",
                                 static_cast<unsigned long long>(command.command_id));
                        SendAck(socket_fd, peer, command, factory_timer::AckResult::NotSynced);
                        xSemaphoreTake(countdown_mutex, portMAX_DELAY);
                        const auto snapshot = countdown.Snapshot();
                        xSemaphoreGive(countdown_mutex);
                        SendStatus(socket_fd, peer, snapshot);
                    } else {
                        int64_t local_start_us = 0;
                        if (command.type == factory_timer::CommandType::StartAt) {
                            if (clock_sync.offset_epoch_disciplined_us > 0 &&
                                clock_sync.estimated_master_epoch_us > 0) {
                                const int64_t target_disciplined_us =
                                    clock_sync.offset_epoch_disciplined_us +
                                    (command.start_at_master_us -
                                     clock_sync.estimated_master_epoch_us);
                                local_start_us =
                                    rtc_discipline_disciplined_to_local_us(
                                        target_disciplined_us);
                            } else {
                                local_start_us = command.start_at_master_us -
                                    clock_sync.master_minus_local_offset_us;
                            }
                        }
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                        if (trace_status_path) status_trace.lock_request_us = esp_timer_get_time();
#endif
                        xSemaphoreTake(countdown_mutex, portMAX_DELAY);
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                        if (trace_status_path) {
                            status_trace.lock_acquired_us = esp_timer_get_time();
                            status_trace.discipline_begin_us = esp_timer_get_time();
                        }
#endif
                        const int64_t receive_disciplined_us =
                            rtc_discipline_local_to_disciplined_us(receive_local_us);
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                        if (trace_status_path) status_trace.discipline_end_us = esp_timer_get_time();
#endif
                        const auto processing = factory_timer::ProcessCommand(
                            countdown, command, receive_local_us, local_start_us,
                            receive_disciplined_us);
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                        if (trace_status_path) {
                            status_trace.process_done_us = esp_timer_get_time();
                            status_trace.receive_disciplined_us = receive_disciplined_us;
                            status_trace.snapshot_state = processing.snapshot.state;
                            FillBoundaryPosition(receive_disciplined_us,
                                                 countdown.StartRunningMicroseconds(),
                                                 processing.snapshot.duration_seconds,
                                                 &status_trace.boundary_index,
                                                 &status_trace.lead_us);
                        }
#endif

                        if (processing.ack_result == factory_timer::AckResult::Accepted) {
                            if (command.type == factory_timer::CommandType::StartAt ||
                                command.type == factory_timer::CommandType::Start) {
                                last_start_health = {};
                                last_start_health.command_id = command.command_id;
                                scheduled_start_metadata.valid = true;
                                scheduled_start_metadata.peer = peer;
                                scheduled_start_metadata.have_peer = true;
                                scheduled_start_metadata.command_id = command.command_id;
                                scheduled_start_metadata.target_master_start_us =
                                    command.type == factory_timer::CommandType::StartAt
                                        ? command.start_at_master_us
                                        : 0;
                                scheduled_start_metadata.master_minus_local_offset_us =
                                    command.type == factory_timer::CommandType::StartAt
                                        ? clock_sync.master_minus_local_offset_us
                                        : 0;
                                scheduled_start_metadata.sync_id =
                                    command.type == factory_timer::CommandType::StartAt
                                        ? clock_sync.sync_id
                                        : 0;
                                scheduled_start_metadata.best_rtt_us =
                                    command.type == factory_timer::CommandType::StartAt
                                        ? clock_sync.best_rtt_us
                                        : 0;
                                scheduled_start_metadata.sync_applied_local_us =
                                    command.type == factory_timer::CommandType::StartAt
                                        ? clock_sync.applied_local_us
                                        : 0;
                                scheduled_start_metadata.sync_estimated_master_us =
                                    command.type == factory_timer::CommandType::StartAt
                                        ? clock_sync.estimated_master_apply_us
                                        : 0;
                                scheduled_start_metadata.source_offset_us =
                                    command.type == factory_timer::CommandType::StartAt
                                        ? clock_sync.source_offset_us
                                        : 0;
                                scheduled_start_metadata.offset_epoch_local_us =
                                    command.type == factory_timer::CommandType::StartAt
                                        ? clock_sync.offset_epoch_local_us
                                        : 0;
                                scheduled_start_metadata.offset_epoch_disciplined_us =
                                    command.type == factory_timer::CommandType::StartAt
                                        ? clock_sync.offset_epoch_disciplined_us
                                        : 0;
                                scheduled_start_metadata.estimated_master_epoch_us =
                                    command.type == factory_timer::CommandType::StartAt
                                        ? clock_sync.estimated_master_epoch_us
                                        : 0;
                                ResetNetworkTimingDiagnostics();
                                ResetNetworkSilentDiagnostics();
                                ArmStartTimerLocked(countdown.ScheduledStartMicroseconds());
#if defined(CONFIG_FACTORY_DISPLAY_TEST) && CONFIG_FACTORY_DISPLAY_TEST
                                factory_display_arm(countdown.ScheduledStartMicroseconds(),
                                                    processing.snapshot.duration_seconds,
                                                    command.command_id);
#endif
                            } else if (command.type == factory_timer::CommandType::Reset) {
                                last_start_health = {};
                                SetStartDiagnosticEdge(false);
#if defined(CONFIG_FACTORY_DISPLAY_TEST) && CONFIG_FACTORY_DISPLAY_TEST
                                factory_display_reset();
#endif
                                // STATUS_REQUEST is intentionally non-mutating. The controller
                                // sends status requests while a countdown is ARMED, and clearing
                                // the metadata here would discard the peer/offset required for
                                // STARTED telemetry even though the deadline task still starts
                                // the countdown correctly.
                                scheduled_start_metadata.valid = false;
                                StopStartTimerLocked();
                            }
                        }
                        xSemaphoreGive(countdown_mutex);
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                        if (trace_status_path) status_trace.lock_release_us = esp_timer_get_time();
#endif

                        if (processing.send_ack) {
                            LogAcceptedCommand(command, processing.ack_result, local_start_us);
                            SendAck(socket_fd, peer, command, processing.ack_result);
                        }
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                        if (trace_status_path) {
                            SendStatus(socket_fd, peer, processing.snapshot, &status_trace.send);
                            StoreStatusTxTrace(StatusSendReason::RequestReply,
                                               processing.snapshot,
                                               status_trace.send);
                            status_trace.handler_exit_us = esp_timer_get_time();
                            if (processing.snapshot.state == factory_timer::TimerState::Running) {
                                StoreStatusPathTrace(status_trace);
                            }
                        } else
#endif
                        {
                            SendStatus(socket_fd, peer, processing.snapshot);
                        }
                        last_snapshot = processing.snapshot;
                        last_heartbeat = receive_local_us;
                    }
                    }
                }
            }
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(kTag, "UDP receive failed: errno=%d", errno);
        }

        TimerStartEvent timer_event{};
        while (xQueueReceive(timer_event_queue, &timer_event, 0) == pdTRUE) {
            const int64_t deferred_log_us = esp_timer_get_time();
            const int64_t start_error_us =
                timer_event.target_master_start_us > 0
                    ? timer_event.estimated_master_start_us - timer_event.target_master_start_us
                    : 0;
            const int64_t scheduler_lateness_us =
                timer_event.actual_local_start_us - timer_event.target_local_start_us;
            last_start_health.valid = timer_event.target_master_start_us > 0;
            last_start_health.command_id = timer_event.snapshot.last_command_id;
            last_start_health.start_error_us = start_error_us;
            last_start_health.scheduler_lateness_us = scheduler_lateness_us;

            ESP_LOGI(kTag,
                     "Countdown started: remaining=%u command=%016llX local_us=%lld disciplined_us=%lld estimated_master_us=%lld scheduler_lateness_us=%lld deferred_log_delay_us=%lld",
                     static_cast<unsigned>(timer_event.snapshot.remaining_seconds),
                     static_cast<unsigned long long>(timer_event.snapshot.last_command_id),
                     static_cast<long long>(timer_event.actual_local_start_us),
                     static_cast<long long>(timer_event.actual_disciplined_start_us),
                     static_cast<long long>(timer_event.estimated_master_start_us),
                     static_cast<long long>(scheduler_lateness_us),
                     static_cast<long long>(deferred_log_us -
                                            timer_event.actual_local_start_us));

#if defined(CONFIG_FACTORY_RTC_QUAL_LOGS) && CONFIG_FACTORY_RTC_QUAL_LOGS
            if (timer_event.target_master_start_us > 0 &&
                timer_event.sync_applied_local_us > 0) {
                const int64_t sync_age_local_us =
                    timer_event.actual_local_start_us -
                    timer_event.sync_applied_local_us;
                ESP_LOGI(kTag,
                         "RTC_START_QUAL device=%s command=%016llX sync=%016llX sync_local_us=%lld sync_estimated_master_us=%lld offset_master_minus_local_us=%lld best_rtt_us=%llu target_master_us=%lld actual_local_us=%lld estimated_master_us=%lld start_error_us=%lld scheduler_lateness_us=%lld sync_age_local_us=%lld disciplined_start_us=%lld source_offset_us=%lld offset_epoch_local_us=%lld offset_epoch_disciplined_us=%lld offset_master_minus_disciplined_us=%lld",
                         CONFIG_FACTORY_DEVICE_ID,
                         static_cast<unsigned long long>(timer_event.snapshot.last_command_id),
                         static_cast<unsigned long long>(timer_event.sync_id),
                         static_cast<long long>(timer_event.sync_applied_local_us),
                         static_cast<long long>(timer_event.sync_estimated_master_us),
                         static_cast<long long>(timer_event.master_minus_local_offset_us),
                         static_cast<unsigned long long>(timer_event.best_rtt_us),
                         static_cast<long long>(timer_event.target_master_start_us),
                         static_cast<long long>(timer_event.actual_local_start_us),
                         static_cast<long long>(timer_event.estimated_master_start_us),
                         static_cast<long long>(start_error_us),
                         static_cast<long long>(scheduler_lateness_us),
                         static_cast<long long>(sync_age_local_us),
                         static_cast<long long>(timer_event.actual_disciplined_start_us),
                         static_cast<long long>(timer_event.source_offset_us),
                         static_cast<long long>(timer_event.offset_epoch_local_us),
                         static_cast<long long>(timer_event.offset_epoch_disciplined_us),
                         static_cast<long long>(
                             timer_event.source_offset_us +
                             timer_event.offset_epoch_local_us -
                             timer_event.offset_epoch_disciplined_us));
            }
#endif

            if (timer_event.have_peer && timer_event.target_master_start_us > 0) {
                SendStarted(socket_fd, timer_event.peer, timer_event.snapshot,
                            timer_event.actual_local_start_us,
                            timer_event.estimated_master_start_us,
                            timer_event.target_master_start_us);
            }
            if (timer_event.have_peer) {
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                SendStatusWithTrace(socket_fd, timer_event.peer, timer_event.snapshot,
                                    StatusSendReason::TimerStarted);
#else
                SendStatus(socket_fd, timer_event.peer, timer_event.snapshot);
#endif
            }

            xSemaphoreTake(countdown_mutex, portMAX_DELAY);
            const auto current = countdown.Snapshot();
            xSemaphoreGive(countdown_mutex);
            if (current.last_command_id == timer_event.snapshot.last_command_id &&
                current.state == factory_timer::TimerState::Running) {
                last_snapshot = current;
                last_heartbeat = timer_event.actual_local_start_us;
            }
        }

        const int64_t now = esp_timer_get_time();
        const int64_t disciplined_now = rtc_discipline_local_to_disciplined_us(now);
        xSemaphoreTake(countdown_mutex, portMAX_DELAY);
        auto snapshot = countdown.Snapshot();
        if (snapshot.state != factory_timer::TimerState::Armed) {
            snapshot = countdown.Update(now, disciplined_now);
        }
        xSemaphoreGive(countdown_mutex);

        if (snapshot != last_snapshot) {
            if (snapshot.state == factory_timer::TimerState::Running &&
                last_snapshot.state != factory_timer::TimerState::Running) {
                // The timer task owns Armed -> Running. This branch can only be
                // reached when its queued telemetry has not yet been consumed.
                FACTORY_DEBUG_LOGI(kTag,
                                   "Observed timer-task start before telemetry drain: command=%016llX",
                                   static_cast<unsigned long long>(snapshot.last_command_id));
            } else if (snapshot.state == factory_timer::TimerState::Running &&
                       snapshot.remaining_seconds != last_snapshot.remaining_seconds) {
                ESP_LOGI(kTag, "Countdown second: %u",
                         static_cast<unsigned>(snapshot.remaining_seconds));
            } else if (snapshot.state == factory_timer::TimerState::Finished) {
                ESP_LOGI(kTag, "Countdown finished: command=%016llX",
                         static_cast<unsigned long long>(snapshot.last_command_id));
                flash_guard_diag_countdown_finished();
                RequestPostRunDiagnosticDump();
            }
            bool status_sent = false;
            if (have_peer) {
                bool send_state_change = true;
#if !defined(CONFIG_FACTORY_PER_SECOND_STATE_CHANGE_STATUS) || !CONFIG_FACTORY_PER_SECOND_STATE_CHANGE_STATUS
                if (snapshot.state == factory_timer::TimerState::Running &&
                    last_snapshot.state == factory_timer::TimerState::Running &&
                    snapshot.remaining_seconds != last_snapshot.remaining_seconds) {
                    send_state_change = false;
                }
#endif
                if (send_state_change) {
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                    SendStatusWithTrace(socket_fd, last_peer, snapshot, StatusSendReason::StateChange);
#else
                    SendStatus(socket_fd, last_peer, snapshot);
#endif
                    status_sent = true;
                } else if (now - last_heartbeat >= kHeartbeatIntervalUs) {
                    if (ShouldSuppressRunningHeartbeat(snapshot)) {
                        // v6.20 production behavior: keep RUNNING heartbeat traffic silent
                        // before Wi-Fi/RTC/status formatting/sendto work begins.
                        RecordSuppressedRunningHeartbeat();
                        last_heartbeat = now;
                    } else {
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                        SendStatusWithTrace(socket_fd, last_peer, snapshot, StatusSendReason::Heartbeat);
#else
                        SendStatus(socket_fd, last_peer, snapshot);
#endif
                        status_sent = true;
                    }
                }
            }
            last_snapshot = snapshot;
            if (status_sent || !have_peer) {
                last_heartbeat = now;
            }
        } else if (have_peer && now - last_heartbeat >= kHeartbeatIntervalUs) {
            if (ShouldSuppressRunningHeartbeat(snapshot)) {
                RecordSuppressedRunningHeartbeat();
            } else {
#if defined(CONFIG_FACTORY_RX_BOUNDARY_TRACE) && CONFIG_FACTORY_RX_BOUNDARY_TRACE
                SendStatusWithTrace(socket_fd, last_peer, snapshot, StatusSendReason::Heartbeat);
#else
                SendStatus(socket_fd, last_peer, snapshot);
#endif
            }
            // Advance cadence even when suppressed so an overdue heartbeat cannot
            // repeatedly re-enter this branch on every CommandTask iteration.
            last_heartbeat = now;
        }
    }
}

} // namespace


extern "C" int factory_timer_lwip_ip4_input_hook(struct pbuf *p, struct netif *inp) {
    (void)inp;
    const int64_t ip_input_us = esp_timer_get_time();
    if (p == nullptr || p->tot_len < (IP_HLEN + UDP_HLEN)) return 0;

    struct ip_hdr iphdr{};
    if (pbuf_copy_partial(p, &iphdr, sizeof(iphdr), 0) != sizeof(iphdr)) return 0;
    if (IPH_V(&iphdr) != 4 || IPH_PROTO(&iphdr) != IPPROTO_UDP) return 0;
    const uint16_t ip_header_len = IPH_HL_BYTES(&iphdr);
    if (ip_header_len < IP_HLEN || p->tot_len < (uint16_t)(ip_header_len + UDP_HLEN)) return 0;

    struct udp_hdr udphdr{};
    if (pbuf_copy_partial(p, &udphdr, sizeof(udphdr), ip_header_len) != sizeof(udphdr)) return 0;
    const uint16_t destination_port = lwip_ntohs(udphdr.dest);
    if (destination_port != kCommandPort) return 0;
    const uint16_t udp_length = lwip_ntohs(udphdr.len);
    if (udp_length < UDP_HLEN) return 0;

    const uint16_t payload_length = (uint16_t)(udp_length - UDP_HLEN);
    const uint64_t payload_fingerprint =
        PbufPayloadFingerprint(p, (uint16_t)(ip_header_len + UDP_HLEN), payload_length);
    if (payload_fingerprint == 0) return 0;

    RecordEarlyIngress(ip_input_us,
                       iphdr.src.addr,
                       lwip_ntohs(udphdr.src),
                       destination_port,
                       payload_length,
                       payload_fingerprint);
    return 0;  // never consume the packet
}

extern "C" void app_main() {
    if (!ValidConfiguredDeviceId(CONFIG_FACTORY_DEVICE_ID)) {
        ESP_LOGE(kTag, "CONFIG_FACTORY_DEVICE_ID is invalid; use 1-16 uppercase letters, digits, '_' or '-'");
        return;
    }
    ESP_LOGI(kTag, "Factory countdown timer starting");
    ESP_LOGI(kTag, "Configured device identity: %s", CONFIG_FACTORY_DEVICE_ID);
    InitialiseNvs();
    flash_guard_diag_init();
    InitialiseDieTemperatureSensor();
    InitialiseRtcDiscipline();
#if defined(CONFIG_FACTORY_RTC_QUAL_LOGS) && CONFIG_FACTORY_RTC_QUAL_LOGS
    if (rtc_discipline_ready) {
        if (xTaskCreatePinnedToCore(&RtcQualificationTask,
                                    "rtc_qual",
                                    4096,
                                    nullptr,
                                    kRtcQualificationTaskPriority,
                                    nullptr,
                                    kControlTaskCore) != pdPASS) {
            ESP_LOGE(kTag, "Could not create RTC qualification logging task");
            ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
        }
    }
#endif
#if defined(CONFIG_FACTORY_DISPLAY_TEST) && CONFIG_FACTORY_DISPLAY_TEST
    if (!factory_display_init(CONFIG_FACTORY_DEVICE_ID,
                              static_cast<uint8_t>(CONFIG_FACTORY_DISPLAY_BRIGHTNESS))) {
        ESP_LOGE(kTag, "Countdown display initialization failed");
        return;
    }
#endif
    InitialiseStartScheduler();
    InitialiseRxBoundaryTrace();
    InitialiseDiagnosticDumpTask();
    InitialiseWifi();
    if (xTaskCreatePinnedToCore(&CommandTask, "udp_command", 7168, nullptr,
                                kCommandTaskPriority, nullptr,
                                kControlTaskCore) != pdPASS) {
        ESP_LOGE(kTag, "Could not create UDP command task");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    ESP_LOGI(kTag,
             "Control-task placement: CommandTask priority=%u core=%d; TimerTask priority=%u core=%d",
             static_cast<unsigned>(kCommandTaskPriority),
             static_cast<int>(kControlTaskCore),
             static_cast<unsigned>(kTimerTaskPriority),
             static_cast<int>(kControlTaskCore));
}
