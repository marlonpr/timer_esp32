#pragma once
#include <cstdint>
#include "esp_err.h"

namespace factory_timer {
struct WifiMaintenanceHooks {
    bool (*segment_active)();
    void (*invalidate_sync)();
};
esp_err_t WifiMaintenanceInit(WifiMaintenanceHooks hooks);
void WifiStationStarted();
void WifiStationConnected();
void WifiStationDisconnected();
void WifiFactoryLinkReady();
void WifiScanCompleted(uint32_t status);
bool WifiBeginControl(int64_t now_us, bool defer_roaming);
void WifiEndControl();
// Held throughout packet processing so START cannot race an idle scan/handover.
class WifiControlLease {
public:
    WifiControlLease(int64_t now_us, bool defer_roaming)
        : ready_(WifiBeginControl(now_us, defer_roaming)) {}
    ~WifiControlLease() { WifiEndControl(); }
    WifiControlLease(const WifiControlLease&) = delete;
    WifiControlLease& operator=(const WifiControlLease&) = delete;
    bool TimingReady() const { return ready_; }
private:
    bool ready_;
};
} // namespace factory_timer
