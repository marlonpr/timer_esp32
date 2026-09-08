#include "command_processor.h"
#include "factory_log.h"
#include "network_policy.h"
#include "protocol_codec.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string_view>

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
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "factory_timer";
constexpr uint16_t kCommandPort = 5000;
constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr int64_t kHeartbeatIntervalUs = 2000000;
constexpr int64_t kCommandSocketTimeoutUs = 50000;
constexpr uint32_t kRejectedNetworkRetryDelayMs = 1000;
constexpr UBaseType_t kTimerTaskPriority = 24;
constexpr int64_t kSchedulerFineLeadUs = 2000;
constexpr int64_t kFreeRtosTickUs = 1000000LL / configTICK_RATE_HZ;

EventGroupHandle_t wifi_event_group;
esp_netif_t *wifi_sta_netif = nullptr;
TaskHandle_t dhcp_retry_task_handle = nullptr;
std::atomic<bool> dhcp_retry_pending{false};
std::atomic<uint32_t> rejected_network_count{0};
factory_timer::CountdownTimer countdown;
SemaphoreHandle_t countdown_mutex = nullptr;
QueueHandle_t timer_event_queue = nullptr;
TaskHandle_t timer_task_handle = nullptr;

struct ClockSyncState {
    std::atomic<bool> valid{false};
    int64_t master_minus_local_offset_us{};
    uint64_t best_rtt_us{};
    uint64_t sync_id{};
};

ClockSyncState clock_sync;

struct ScheduledStartMetadata {
    bool valid{};
    sockaddr_in peer{};
    bool have_peer{};
    uint64_t command_id{};
    int64_t target_master_start_us{};
    int64_t master_minus_local_offset_us{};
};

struct TimerStartEvent {
    factory_timer::TimerSnapshot snapshot{};
    sockaddr_in peer{};
    bool have_peer{};
    int64_t actual_local_start_us{};
    int64_t estimated_master_start_us{-1};
    int64_t target_master_start_us{};
};

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
                     "; expected 192.168.0.0/16 with gateway 192.168.0.1 (attempt=%u)",
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
                int length, const char *description) {
    if (length <= 0 ||
        static_cast<std::size_t>(length) > factory_timer::kMaxPacketLength) {
        ESP_LOGE(kTag, "Could not format %s packet", description);
        return false;
    }
    const int sent = sendto(socket_fd, packet, length, 0,
                            reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
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

void SendStatus(int socket_fd, const sockaddr_in &peer,
                const factory_timer::TimerSnapshot &snapshot) {
    char packet[factory_timer::kMaxPacketLength + 1]{};
    const int length = factory_timer::FormatStatus(
        packet, sizeof(packet), CONFIG_FACTORY_DEVICE_ID,
        snapshot.last_command_id, snapshot.state, snapshot.remaining_seconds);
    SendPacket(socket_fd, peer, packet, length, "STATUS");
}

void SendSyncReply(int socket_fd, const sockaddr_in &peer,
                   const factory_timer::SyncRequestPacket &request,
                   int64_t local_t2_us) {
    // t3 is intentionally captured BEFORE the optional test delay. This makes
    // the delay appear in (t4 - t3), exactly like reverse-path network latency.
    const int64_t local_t3_us = esp_timer_get_time();
    char packet[factory_timer::kMaxPacketLength + 1]{};
    const int length = factory_timer::FormatSyncReply(
        packet, sizeof(packet), CONFIG_FACTORY_DEVICE_ID, request.sync_id,
        request.master_t1_us, local_t2_us, local_t3_us);

    if (request.artificial_reply_delay_us > 0) {
        const uint32_t delay_ms =
            (request.artificial_reply_delay_us + 999U) / 1000U;
        ESP_LOGI(kTag,
                 "SYNC experiment reverse-path delay: sync=%016llX delay_us=%u t3_local_us=%lld",
                 static_cast<unsigned long long>(request.sync_id),
                 static_cast<unsigned>(request.artificial_reply_delay_us),
                 static_cast<long long>(local_t3_us));
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    SendPacket(socket_fd, peer, packet, length, "SYNC_REPLY");
}

void SendSyncApplied(int socket_fd, const sockaddr_in &peer,
                     const factory_timer::SyncSetPacket &sync_set) {
    char packet[factory_timer::kMaxPacketLength + 1]{};
    const int length = factory_timer::FormatSyncApplied(
        packet, sizeof(packet), CONFIG_FACTORY_DEVICE_ID, sync_set.sync_id,
        sync_set.master_minus_local_offset_us, sync_set.best_rtt_us);
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

            xSemaphoreTake(countdown_mutex, portMAX_DELAY);
            const auto before = countdown.Snapshot();
            const auto after = countdown.Update(now);

            if (before.state == factory_timer::TimerState::Armed &&
                after.state == factory_timer::TimerState::Running) {
                event.snapshot = after;
                event.actual_local_start_us = now;

                if (scheduled_start_metadata.valid &&
                    scheduled_start_metadata.command_id == after.last_command_id) {
                    event.peer = scheduled_start_metadata.peer;
                    event.have_peer = scheduled_start_metadata.have_peer;
                    event.target_master_start_us =
                        scheduled_start_metadata.target_master_start_us;
                    if (event.target_master_start_us > 0) {
                        event.estimated_master_start_us =
                            now + scheduled_start_metadata.master_minus_local_offset_us;
                    }
                }

                scheduled_start_metadata.valid = false;
                started = true;
            }
            xSemaphoreGive(countdown_mutex);

            if (!started) {
                // State or target changed on the other core while we were in
                // the short final window. Re-read it instead of starting a
                // stale command.
                continue;
            }

            ESP_LOGI(kTag,
                     "Countdown started: remaining=%u command=%016llX local_us=%lld estimated_master_us=%lld scheduler_lateness_us=%lld",
                     static_cast<unsigned>(event.snapshot.remaining_seconds),
                     static_cast<unsigned long long>(event.snapshot.last_command_id),
                     static_cast<long long>(event.actual_local_start_us),
                     static_cast<long long>(event.estimated_master_start_us),
                     static_cast<long long>(event.actual_local_start_us - target_local_us));

            if (xQueueSend(timer_event_queue, &event, 0) != pdTRUE) {
                ESP_LOGW(kTag,
                         "Timer event queue full; STARTED telemetry may be missing for command=%016llX",
                         static_cast<unsigned long long>(event.snapshot.last_command_id));
            }
            break;
        }
    }
}

void InitialiseStartScheduler() {
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

    if (xTaskCreate(&TimerTask, "countdown_timer", 4096, nullptr,
                    kTimerTaskPriority, &timer_task_handle) != pdPASS) {
        ESP_LOGE(kTag, "Could not create countdown timer task");
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    ESP_LOGI(kTag,
             "High-resolution countdown scheduler ready; source=deadline_task final_spin<=~%lld us timer_task_priority=%u UDP receive timeout=%lld us",
             static_cast<long long>(kFreeRtosTickUs + kSchedulerFineLeadUs),
             static_cast<unsigned>(kTimerTaskPriority),
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

        if (received > 0) {
            if (received > static_cast<int>(factory_timer::kMaxPacketLength)) {
                ESP_LOGW(kTag, "Discarded oversized UDP datagram");
            } else {
                const std::string_view text(buffer, static_cast<std::size_t>(received));
                factory_timer::MasterPacket packet{};
                factory_timer::ParseError parse_error{};
                if (!factory_timer::ParseMasterPacket(text, packet, parse_error)) {
                    ESP_LOGW(kTag, "Discarded invalid packet (%s)",
                             factory_timer::ParseErrorName(parse_error));
                } else if (packet.type == factory_timer::MasterPacketType::SyncRequest) {
                    SendSyncReply(socket_fd, peer, packet.sync_request, receive_local_us);
                    FACTORY_DEBUG_LOGI(kTag,
                                       "SYNC id=%016llX t1=%lld t2=%lld artificial_reply_delay_us=%u",
                                       static_cast<unsigned long long>(packet.sync_request.sync_id),
                                       static_cast<long long>(packet.sync_request.master_t1_us),
                                       static_cast<long long>(receive_local_us),
                                       static_cast<unsigned>(packet.sync_request.artificial_reply_delay_us));
                } else if (packet.type == factory_timer::MasterPacketType::SyncSet) {
                    clock_sync.master_minus_local_offset_us =
                        packet.sync_set.master_minus_local_offset_us;
                    clock_sync.best_rtt_us = packet.sync_set.best_rtt_us;
                    clock_sync.sync_id = packet.sync_set.sync_id;
                    clock_sync.valid.store(true, std::memory_order_release);
                    SendSyncApplied(socket_fd, peer, packet.sync_set);
                    ESP_LOGI(kTag,
                             "Clock synchronized: offset_master_minus_local_us=%lld best_rtt_us=%llu sync=%016llX",
                             static_cast<long long>(clock_sync.master_minus_local_offset_us),
                             static_cast<unsigned long long>(clock_sync.best_rtt_us),
                             static_cast<unsigned long long>(clock_sync.sync_id));
                } else {
                    const auto &command = packet.command;
                    last_peer = peer;
                    have_peer = true;

                    if (command.type == factory_timer::CommandType::StartAt &&
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
                            local_start_us = command.start_at_master_us -
                                clock_sync.master_minus_local_offset_us;
                        }
                        xSemaphoreTake(countdown_mutex, portMAX_DELAY);
                        const auto processing = factory_timer::ProcessCommand(
                            countdown, command, receive_local_us, local_start_us);

                        if (processing.ack_result == factory_timer::AckResult::Accepted) {
                            if (command.type == factory_timer::CommandType::StartAt ||
                                command.type == factory_timer::CommandType::Start) {
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
                                ArmStartTimerLocked(countdown.ScheduledStartMicroseconds());
                            } else if (command.type == factory_timer::CommandType::Reset) {
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

                        if (processing.send_ack) {
                            LogAcceptedCommand(command, processing.ack_result, local_start_us);
                            SendAck(socket_fd, peer, command, processing.ack_result);
                        }
                        SendStatus(socket_fd, peer, processing.snapshot);
                        last_snapshot = processing.snapshot;
                        last_heartbeat = receive_local_us;
                    }
                }
            }
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(kTag, "UDP receive failed: errno=%d", errno);
        }

        TimerStartEvent timer_event{};
        while (xQueueReceive(timer_event_queue, &timer_event, 0) == pdTRUE) {
            if (timer_event.have_peer && timer_event.target_master_start_us > 0) {
                SendStarted(socket_fd, timer_event.peer, timer_event.snapshot,
                            timer_event.actual_local_start_us,
                            timer_event.estimated_master_start_us,
                            timer_event.target_master_start_us);
            }
            if (timer_event.have_peer) {
                SendStatus(socket_fd, timer_event.peer, timer_event.snapshot);
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
        xSemaphoreTake(countdown_mutex, portMAX_DELAY);
        auto snapshot = countdown.Snapshot();
        if (snapshot.state != factory_timer::TimerState::Armed) {
            snapshot = countdown.Update(now);
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
            }
            if (have_peer) SendStatus(socket_fd, last_peer, snapshot);
            last_snapshot = snapshot;
            last_heartbeat = now;
        } else if (have_peer && now - last_heartbeat >= kHeartbeatIntervalUs) {
            SendStatus(socket_fd, last_peer, snapshot);
            last_heartbeat = now;
        }
    }
}

} // namespace

extern "C" void app_main() {
    if (!ValidConfiguredDeviceId(CONFIG_FACTORY_DEVICE_ID)) {
        ESP_LOGE(kTag, "CONFIG_FACTORY_DEVICE_ID is invalid; use 1-16 uppercase letters, digits, '_' or '-'");
        return;
    }
    ESP_LOGI(kTag, "Factory countdown timer starting");
    ESP_LOGI(kTag, "Configured device identity: %s", CONFIG_FACTORY_DEVICE_ID);
    InitialiseNvs();
    InitialiseStartScheduler();
    InitialiseWifi();
    xTaskCreate(&CommandTask, "udp_command", 7168, nullptr, 5, nullptr);
}
