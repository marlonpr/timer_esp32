#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=0x101, ESP_ERR_INVALID_ARG=0x102, ESP_ERR_INVALID_STATE=0x103;
constexpr int WIFI_IF_STA=0, WIFI_SCAN_TYPE_ACTIVE=0, WIFI_AUTH_OPEN=0, WIFI_AUTH_WPA2_PSK=3;
constexpr int pdTRUE=1, pdPASS=1, portMAX_DELAY=-1;
#define pdMS_TO_TICKS(value) (value)
using SemaphoreHandle_t = std::mutex*;
using TaskHandle_t = void*;
struct wifi_config_t {
    struct {
        uint8_t ssid[32]{}, password[64]{}, bssid[6]{};
        bool bssid_set{};
        struct { int authmode{}; } threshold;
    } sta;
};
struct wifi_ap_record_t { uint8_t bssid[6]{},ssid[33]{}; int8_t rssi{}; int authmode{}; };
struct wifi_scan_config_t {
    uint8_t* ssid{};
    uint8_t channel{};
    int scan_type{};
    struct { struct { uint32_t min{},max{}; } active; } scan_time;
};
struct HostWifi {
    wifi_config_t config{};
    wifi_ap_record_t current{};
    std::vector<wifi_ap_record_t> records;
    std::atomic<unsigned> connects{0}, scans{0}, stops{0}, clears{0}, disconnects{0}, configs{0}, ap_reads{0};
    esp_err_t connect_result{}, scan_result{}, stop_result{}, records_result{}, config_result{};
    uint32_t random_value=500000;
    int64_t now_us{};
    void (*disconnected_callback)(){};
    void (*scan_done_callback)(uint32_t){};
    bool auto_scan_stop_done=true;
    bool list_allocated{};
} host;
struct EndHostTick {};
inline thread_local unsigned host_tick_waits;
inline thread_local bool host_mutex_owned;
inline int64_t esp_timer_get_time() { return host.now_us; }
inline uint32_t esp_random() { return host.random_value; }
inline const char* esp_err_to_name(esp_err_t) { return "HOST_ERROR"; }
inline void wifi_host_log(const char*, const char*, ...) {}
#define ESP_LOGI(...) wifi_host_log(__VA_ARGS__)
#define ESP_LOGW(...) wifi_host_log(__VA_ARGS__)
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex; }
inline int xSemaphoreTake(SemaphoreHandle_t mutex,int) { mutex->lock(); host_mutex_owned=true; return 1; }
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { host_mutex_owned=false; mutex->unlock(); }
inline void vSemaphoreDelete(SemaphoreHandle_t mutex) { delete mutex; }
inline int xTaskCreatePinnedToCore(void(*)(void*),const char*,unsigned stack,void*,unsigned priority,TaskHandle_t* out,int core) {
    assert(stack>=4096 && priority==2 && core==0); *out=reinterpret_cast<void*>(1); return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t) {}
inline unsigned ulTaskNotifyTake(int,int ticks) {
    assert(ticks==100);
    if(host_tick_waits++) throw EndHostTick{};
    return 0;
}
inline esp_err_t esp_wifi_get_config(int,const wifi_config_t*) = delete;
inline esp_err_t esp_wifi_get_config(int,wifi_config_t* out) { *out=host.config;return ESP_OK; }
inline esp_err_t esp_wifi_set_config(int,const wifi_config_t* in) {
    assert(host_mutex_owned); ++host.configs;
    if(host.config_result==ESP_OK) host.config=*in;
    return host.config_result;
}
inline esp_err_t esp_wifi_connect() { assert(host_mutex_owned);++host.connects;return host.connect_result; }
inline esp_err_t esp_wifi_disconnect() {
    assert(host_mutex_owned);++host.disconnects;
    if(host.disconnected_callback) host.disconnected_callback();
    return ESP_OK;
}
inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* out) {
    assert(host_mutex_owned);++host.ap_reads;*out=host.current;return ESP_OK;
}
inline esp_err_t esp_wifi_scan_start(const wifi_scan_config_t* scan,bool block) {
    assert(host_mutex_owned && !block && scan->channel==0 && scan->scan_time.active.max==120);
    assert(std::strncmp(reinterpret_cast<char*>(scan->ssid),reinterpret_cast<char*>(host.config.sta.ssid),32)==0);
    ++host.scans;
    if(host.scan_result==ESP_OK) host.list_allocated=true;
    return host.scan_result;
}
inline esp_err_t esp_wifi_scan_stop() {
    assert(host_mutex_owned);++host.stops;
    if(host.stop_result==ESP_OK && host.auto_scan_stop_done && host.scan_done_callback) host.scan_done_callback(1);
    return host.stop_result;
}
inline esp_err_t esp_wifi_clear_ap_list() { assert(host_mutex_owned);++host.clears;host.list_allocated=false;return ESP_OK; }
inline esp_err_t esp_wifi_scan_get_ap_records(uint16_t* count,wifi_ap_record_t* out) {
    assert(host_mutex_owned);
    if(host.records_result!=ESP_OK) return host.records_result;
    *count=static_cast<uint16_t>(std::min<size_t>(*count,host.records.size()));
    std::copy_n(host.records.begin(),*count,out);host.list_allocated=false;return ESP_OK;
}
