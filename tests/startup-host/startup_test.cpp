// Execute verbatim main.cpp boot/configuration/gate functions against host APIs.
// Network association and the RTC estimator are simulated, not SDK emulation.
#include "rtc_discipline.h"
#include "network_policy.h"
#include "wifi_maintenance.h"
#include "fake_i2c.h"
#include "esp_log.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#define CONFIG_FACTORY_DEVICE_ID "ESP07"
#define CONFIG_FACTORY_WIFI_SSID "host-test-ssid"
#define CONFIG_FACTORY_WIFI_PASSWORD "host-test-password"
#define CONFIG_FACTORY_WIFI_BSSID "AA:BB:CC:DD:EE:FE"
#define CONFIG_FACTORY_DS3231_DISCIPLINE 1
#define CONFIG_FACTORY_DS3231_SQW_GPIO 27
#define CONFIG_FACTORY_RTC_QUAL_LOGS 1
#define CONFIG_FACTORY_DISPLAY_TEST 1
#define CONFIG_FACTORY_DISPLAY_BRIGHTNESS 50
#define ESP_ERROR_CHECK(value) assert((value) == ESP_OK)
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
constexpr int pdPASS = 1, ESP_EVENT_ANY_ID = -1, WIFI_EVENT = 1, IP_EVENT = 2;
constexpr int IP_EVENT_STA_GOT_IP = 3, WIFI_IF_STA = 0, WIFI_MODE_STA = 1, WIFI_PS_NONE = 0;
constexpr int WIFI_STORAGE_RAM = 1;
constexpr int WIFI_ALL_CHANNEL_SCAN = 1, WIFI_CONNECT_AP_BY_SIGNAL = 0;
constexpr int WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3;
constexpr int kControlTaskCore = 0, kCommandTaskPriority = 4, kTimerTaskPriority = 5;
constexpr int kRtcQualificationTaskPriority = 2, kRtcStartMinimumFitPoints = 129;
constexpr double kRtcStartMaximumFitRmsUs = 3.0;
static const char *kTag = "startup_host";
static int token;
using TaskHandle_t = void *;
static TaskHandle_t dhcp_retry_task_handle;
static void *wifi_event_group, *wifi_sta_netif;
static uint64_t device_boot_id, rtc_recovered_edges;
static bool rtc_discipline_ready, display_initialized, wifi_started, command_started;
static unsigned rtc_tasks;
static ds3231_dev_t ds3231_device;
static rtc_discipline_status_t rtc_status;
struct RecoveryWindow {
    unsigned Observe(uint64_t, uint64_t, uint64_t, uint32_t, bool) { return 129; }
} rtc_recovery_window;
struct wifi_init_config_t {};
struct wifi_config_t {
    struct {
        uint8_t ssid[32], password[64];
        struct { int authmode; } threshold;
        int scan_method, sort_method;
        struct { bool capable, required; } pmf_cfg;
        uint8_t bssid[6], channel;
        bool bssid_set;
    } sta;
};
static wifi_config_t configured_wifi;
namespace factory_timer { uint8_t BuildDiagnosticFlags() { return 0; } }
static bool WifiSegmentActive() { return false; }
static void InvalidateWifiSynchronization() {}
namespace factory_timer {
esp_err_t WifiMaintenanceInit(WifiMaintenanceHooks hooks) {
    assert(hooks.segment_active && hooks.invalidate_sync);
    return ESP_OK;
}
}
static uint32_t esp_random() { return 1; }
static bool ValidConfiguredDeviceId(const char *id) { return std::strcmp(id, "ESP07") == 0; }
static void InitialiseNvs() {}
static void flash_guard_diag_init() {}
static void InitialiseDieTemperatureSensor() {}
static void InitialiseStartScheduler() {}
static void InitialiseRxBoundaryTrace() {}
static void InitialiseDiagnosticDumpTask() {}
static void RtcQualificationTask(void *) {}
static void DhcpRetryTask(void *) {}
static void CommandTask(void *) {}
static bool factory_display_init(const char *id, uint8_t brightness) {
    assert(std::strcmp(id, "ESP07") == 0 && brightness == 50);
    display_initialized = true;
    return true;
}
static int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, TaskHandle_t *out) {
    if (out) *out = &token;
    return pdPASS;
}
static int xTaskCreatePinnedToCore(void (*)(void *), const char *name, unsigned, void *, unsigned, TaskHandle_t *, int) {
    if (std::strcmp(name, "rtc_qual") == 0) ++rtc_tasks;
    if (std::strcmp(name, "udp_command") == 0) command_started = true;
    return pdPASS;
}
static void *xEventGroupCreate() { return &token; }
static esp_err_t esp_netif_init() { return ESP_OK; }
static esp_err_t esp_event_loop_create_default() { return ESP_OK; }
static void *esp_netif_create_default_wifi_sta() { return &token; }
static esp_err_t esp_netif_set_hostname(void *, const char *id) {
    assert(std::strcmp(id, "ESP07") == 0); return ESP_OK;
}
static esp_err_t esp_wifi_init(wifi_init_config_t *) { assert(display_initialized); return ESP_OK; }
static void WifiEventHandler(void *, int, int32_t, void *) {}
static esp_err_t esp_event_handler_register(int, int, decltype(&WifiEventHandler), void *) { return ESP_OK; }
static esp_err_t esp_wifi_set_mode(int mode) { assert(mode == WIFI_MODE_STA); return ESP_OK; }
static esp_err_t esp_wifi_set_storage(int mode) { assert(mode == WIFI_STORAGE_RAM); return ESP_OK; }
static esp_err_t esp_wifi_set_config(int iface, const wifi_config_t *config) {
    assert(iface == WIFI_IF_STA); configured_wifi = *config; return ESP_OK;
}
static esp_err_t esp_wifi_start() { wifi_started = true; return ESP_OK; }
static esp_err_t esp_wifi_set_ps(int mode) { assert(mode == WIFI_PS_NONE); return ESP_OK; }

extern "C" esp_err_t rtc_discipline_init(ds3231_dev_t *rtc, const rtc_discipline_config_t *config)
{
    assert(config->sqw_gpio == 27 && config->fit_points == 129);
    // Model estimator startup using the actual driver's SQW/status/temperature calls.
    esp_err_t err = ds3231_configure_1hz_sqw(rtc);
    if (err != ESP_OK) return err;
    bool osf;
    err = ds3231_get_oscillator_stop_flag(rtc, &osf);
    if (err != ESP_OK) return err;
    float temperature;
    return ds3231_get_temperature_c(rtc, &temperature);
}
extern "C" void rtc_discipline_get_status(rtc_discipline_status_t *out) { *out = rtc_status; }

#include "startup_functions.inc"

int main()
{
    for (unsigned fault = 0; fault < 7; ++fault) {
        fake_i2c_reset();
        rtc_discipline_ready = display_initialized = wifi_started = command_started = false;
        ds3231_device = {};
        rtc_status = {};
        rtc_tasks = 0;
        if (fault == 1) fake_i2c.probe_result = ESP_ERR_NOT_FOUND;
        if (fault == 2) fake_i2c.probe_result = ESP_ERR_TIMEOUT;
        if (fault == 3) fake_i2c.create_result = ESP_ERR_NO_MEM;
        if (fault == 4) fake_i2c.add_result = ESP_ERR_NO_MEM;
        if (fault == 5) fake_i2c.read_result = ESP_ERR_TIMEOUT;
        if (fault == 6) fake_i2c.write_result = ESP_ERR_TIMEOUT;
        app_main();
        assert(display_initialized && wifi_started && command_started);
        assert(rtc_discipline_ready == (fault == 0) && rtc_tasks == (fault == 0 ? 1u : 0u));
        assert(fake_i2c.simulated_wait_ms <= 100);
        assert(configured_wifi.sta.scan_method == WIFI_ALL_CHANNEL_SCAN);
        assert(configured_wifi.sta.sort_method == WIFI_CONNECT_AP_BY_SIGNAL);
        assert(configured_wifi.sta.channel == 0);
        assert(configured_wifi.sta.threshold.authmode == WIFI_AUTH_WPA2_PSK);
        assert(std::strcmp(reinterpret_cast<char *>(configured_wifi.sta.ssid), CONFIG_FACTORY_WIFI_SSID) == 0);
        assert(std::strcmp(reinterpret_cast<char *>(configured_wifi.sta.password), CONFIG_FACTORY_WIFI_PASSWORD) == 0);
#if CONFIG_FACTORY_WIFI_BSSID_LOCK
        const uint8_t expected[] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xfe};
        assert(configured_wifi.sta.bssid_set && std::memcmp(expected, configured_wifi.sta.bssid, 6) == 0);
#else
        assert(!configured_wifi.sta.bssid_set);
#endif
        // Neither an uninitialized nor an acquiring RTC may pass either START gate.
        assert(!RtcRunGateQualified(nullptr, false) && !RtcRunGateQualified(nullptr, true));
        if (fault == 0) {
            rtc_status.state = RTC_DISCIPLINE_LOCKED;
            rtc_status.accepted_edges = 1000;
            rtc_status.fit_point_count = 129;
            rtc_status.fit_rms_us = 0.5;
            rtc_status.rtc_temperature_valid = true;
            assert(RtcRunGateQualified(nullptr, false) && RtcRunGateQualified(nullptr, true));
        }
    }
    std::printf("STARTUP_PATH=PASS 7 scenarios; display/network continue; RTC failure blocks START; BSSID_LOCK=%d\n",
                CONFIG_FACTORY_WIFI_BSSID_LOCK);
}
