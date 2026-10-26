#include "wifi_maintenance.h"
#include "sdkconfig.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace factory_timer {
namespace {
constexpr char kTag[] = "wifi_maintenance";
constexpr int64_t kRoamIntervalUs = 60000000;
constexpr int64_t kControlQuietUs = 30000000;
constexpr int64_t kScanTimeoutUs = 5000000;
constexpr int kWeakRssiDbm = -70;
constexpr int kRoamImprovementDb = 10;
#if defined(CONFIG_FACTORY_WIFI_IDLE_ROAM) && CONFIG_FACTORY_WIFI_IDLE_ROAM
constexpr bool kIdleRoamingEnabled = true;
#else
constexpr bool kIdleRoamingEnabled = false;
#endif
struct Context {
    WifiMaintenanceHooks hooks{};
    SemaphoreHandle_t mutex{};
    TaskHandle_t task{};
    wifi_config_t base_config{};
    std::atomic<bool> started{false}, associated{false}, ready{false};
    std::atomic<uint32_t> disconnects{0};
    std::atomic<bool> scan_done{false};
    std::atomic<uint32_t> scan_status{0};
    bool need_connect{}, connecting{}, had_ready{}, was_active{};
    bool scanning{}, scan_stop_requested{}, roaming{}, target_pending{}, pin_in_use{}, awaiting_disconnect{};
    uint32_t failures{};
    int64_t reconnect_due_us{}, next_roam_us{}, quiet_until_us{}, scan_started_us{};
    uint8_t target_bssid[6]{};
    wifi_ap_record_t records[16]{};
} ctx;

void Wake() { if (ctx.task) xTaskNotifyGive(ctx.task); }

void ScheduleFailure(int64_t now_us) {
    ctx.failures = std::min(ctx.failures + 1, uint32_t{1000});
    // Two prompt retries after the initial attempt, then 3-4 s pauses.
    const int64_t delay_us = ctx.failures < 3 ? 0 : 3000000 + esp_random() % 1000001;
    ctx.reconnect_due_us = now_us + delay_us;
    ctx.need_connect = true;
    ctx.connecting = false;
    ESP_LOGW(kTag, "Reconnect scheduled: failures=%u delay_ms=%u",
             static_cast<unsigned>(ctx.failures), static_cast<unsigned>(delay_us / 1000));
}

bool StopScan(int64_t now_us) {
    if (!ctx.scanning) return true;
    if (ctx.scan_stop_requested) return false;
    const esp_err_t result = esp_wifi_scan_stop();
    if (result != ESP_OK) {
        // Keep the timing barrier closed until stop succeeds or SCAN_DONE proves
        // completion. Never assume a failed stop made the radio quiet.
        ESP_LOGW(kTag, "Idle scan stop pending: %s", esp_err_to_name(result));
        return false;
    }
    // ESP_OK accepts the stop; SCAN_DONE is the completion barrier. Keep START
    // blocked even when event delivery is delayed, and discard partial results.
    ctx.scan_stop_requested = true;
    ctx.next_roam_us = now_us + kRoamIntervalUs;
    ESP_LOGI(kTag, "Idle scan stop requested; waiting for SCAN_DONE before timing preparation");
    return false;
}

void Connect(int64_t now_us) {
    if (ctx.target_pending || ctx.pin_in_use) {
        wifi_config_t config = ctx.base_config;
        if (ctx.target_pending) {
            config.sta.bssid_set = true;
            std::memcpy(config.sta.bssid, ctx.target_bssid, 6);
        }
        const esp_err_t configured = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (configured != ESP_OK) {
            ctx.target_pending = false;
            ctx.pin_in_use = true; // Restore the base policy even if the driver partly changed configuration.
            ScheduleFailure(now_us);
            return;
        }
        ctx.pin_in_use = ctx.target_pending;
        ctx.target_pending = false; // Failed first attempt restores the home SSID policy.
    }
    ctx.need_connect = false;
    const esp_err_t result = esp_wifi_connect();
    if (result == ESP_OK) ctx.connecting = true;
    else ScheduleFailure(now_us);
}

void FinishScan(int64_t now_us, bool active) {
    const bool cancelled = ctx.scan_stop_requested;
    ctx.scanning = false;
    ctx.scan_stop_requested = false;
    ctx.next_roam_us = now_us + kRoamIntervalUs;
    if (cancelled || ctx.scan_status.load() != 0 || active || now_us < ctx.quiet_until_us || !ctx.ready.load()) {
        (void)esp_wifi_clear_ap_list();
        return;
    }
    uint16_t count = sizeof(ctx.records) / sizeof(ctx.records[0]);
    if (esp_wifi_scan_get_ap_records(&count, ctx.records) != ESP_OK) {
        (void)esp_wifi_clear_ap_list();
        return;
    }
    wifi_ap_record_t current{};
    if (esp_wifi_sta_get_ap_info(&current) != ESP_OK || current.rssi >= kWeakRssiDbm) return;
    const wifi_ap_record_t* best = nullptr;
    for (uint16_t index = 0; index < count; ++index) {
        const auto& candidate = ctx.records[index];
        if (std::strncmp(reinterpret_cast<const char*>(candidate.ssid),
                         reinterpret_cast<const char*>(ctx.base_config.sta.ssid), 32) != 0 ||
            std::memcmp(candidate.bssid, current.bssid, 6) == 0 ||
            candidate.authmode < ctx.base_config.sta.threshold.authmode ||
            candidate.rssi < current.rssi + kRoamImprovementDb) continue;
        if (!best || candidate.rssi > best->rssi) best = &candidate;
    }
    if (!best) return;
    std::memcpy(ctx.target_bssid, best->bssid, 6);
    ctx.target_pending = true;
    ctx.roaming = true;
    ctx.awaiting_disconnect = true;
    ctx.hooks.invalidate_sync();
    ESP_LOGI(kTag, "Idle roam: rssi=%d candidate=%d improvement_db=%d; fresh sync required",
             current.rssi, best->rssi, best->rssi - current.rssi);
    // The control mutex stays held through this decision. START/SYNC cannot
    // race the handover or reuse the old AP's synchronization epoch.
    if (esp_wifi_disconnect() != ESP_OK)
        ctx.target_pending = ctx.roaming = ctx.awaiting_disconnect = false;
}

void StepLocked(int64_t now_us) {
    const bool active = ctx.hooks.segment_active();
    if (ctx.was_active && !active) ctx.quiet_until_us = std::max(ctx.quiet_until_us, now_us + kControlQuietUs);
    ctx.was_active = active;
    const uint32_t disconnected = ctx.disconnects.exchange(0);
    if (disconnected) {
        (void)StopScan(now_us);
        ctx.awaiting_disconnect = false;
        ctx.had_ready = ctx.connecting = false;
        if (ctx.target_pending) { ctx.need_connect = true; ctx.reconnect_due_us = now_us; }
        else for (uint32_t n = 0; n < disconnected; ++n) ScheduleFailure(now_us);
    }
    if (ctx.scan_done.exchange(false) && ctx.scanning) FinishScan(now_us, active);
    if (ctx.scanning && (active || now_us < ctx.quiet_until_us || !ctx.ready.load() ||
                         now_us - ctx.scan_started_us >= kScanTimeoutUs)) (void)StopScan(now_us);
    if (ctx.ready.load()) {
        if (!ctx.had_ready) {
            ctx.had_ready = true;
            ctx.failures = 0;
            ctx.need_connect = ctx.connecting = ctx.roaming = false;
            ctx.next_roam_us = now_us + kRoamIntervalUs;
        }
        if (!kIdleRoamingEnabled || active || ctx.scanning || ctx.roaming || ctx.base_config.sta.bssid_set ||
            now_us < ctx.quiet_until_us || now_us < ctx.next_roam_us) return;
        ctx.next_roam_us = now_us + kRoamIntervalUs;
        wifi_ap_record_t current{};
        if (esp_wifi_sta_get_ap_info(&current) != ESP_OK || current.rssi >= kWeakRssiDbm) return;
        wifi_scan_config_t scan{};
        scan.ssid = ctx.base_config.sta.ssid;
        scan.channel = 0;
        scan.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        scan.scan_time.active.max = 120;
        ctx.scan_done.store(false);
        if (esp_wifi_scan_start(&scan, false) == ESP_OK) {
            ctx.scanning = true;
            ctx.scan_started_us = now_us;
            ESP_LOGI(kTag, "Idle scan started: rssi=%d threshold=%d", current.rssi, kWeakRssiDbm);
        }
        return;
    }
    if (ctx.started.load() && !ctx.associated.load() && !ctx.scanning && !ctx.connecting && !ctx.awaiting_disconnect) {
        if (!ctx.need_connect) { ctx.need_connect = true; ctx.reconnect_due_us = now_us; }
        if (now_us >= ctx.reconnect_due_us) Connect(now_us);
    }
}

void MaintenanceTask(void*) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        xSemaphoreTake(ctx.mutex, portMAX_DELAY);
        StepLocked(esp_timer_get_time());
        xSemaphoreGive(ctx.mutex);
    }
}
} // namespace

esp_err_t WifiMaintenanceInit(WifiMaintenanceHooks hooks) {
    if (!hooks.segment_active || !hooks.invalidate_sync) return ESP_ERR_INVALID_ARG;
    if (ctx.mutex) return ESP_ERR_INVALID_STATE;
    const esp_err_t result = esp_wifi_get_config(WIFI_IF_STA, &ctx.base_config);
    if (result != ESP_OK) return result;
    ctx.hooks = hooks;
    ctx.mutex = xSemaphoreCreateMutex();
    if (!ctx.mutex) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(MaintenanceTask, "wifi_maint", 6144, nullptr, 2, &ctx.task, 0) != pdPASS) {
        vSemaphoreDelete(ctx.mutex); ctx.mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void WifiStationStarted() { ctx.started.store(true); Wake(); }
void WifiStationConnected() { ctx.associated.store(true); Wake(); }
void WifiStationDisconnected() {
    ctx.associated.store(false); ctx.ready.store(false);
    ctx.disconnects.fetch_add(1); Wake();
}
void WifiFactoryLinkReady() { ctx.associated.store(true); ctx.ready.store(true); Wake(); }
void WifiScanCompleted(uint32_t status) { ctx.scan_status.store(status); ctx.scan_done.store(true); Wake(); }
bool WifiBeginControl(int64_t now_us, bool defer_roaming) {
    xSemaphoreTake(ctx.mutex, portMAX_DELAY);
    if (defer_roaming) { ctx.quiet_until_us = std::max(ctx.quiet_until_us, now_us + kControlQuietUs); Wake(); }
    return ctx.ready.load() && !ctx.scanning && !ctx.roaming;
}
void WifiEndControl() { xSemaphoreGive(ctx.mutex); }
} // namespace factory_timer
