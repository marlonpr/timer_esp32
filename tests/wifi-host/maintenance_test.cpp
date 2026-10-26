#include "wifi_host.h"
#include "countdown_timer.h"
#include <future>
#include <iostream>
#include <new>
#include <thread>
#ifndef FACTORY_WIFI_SOURCE_PATH
#define FACTORY_WIFI_SOURCE_PATH "../../main/wifi_maintenance.cpp"
#endif
#include FACTORY_WIFI_SOURCE_PATH

using namespace factory_timer;
static CountdownTimer timer;
static unsigned invalidations;
static bool Active() { const auto s=timer.Snapshot().state;return s==TimerState::Armed||s==TimerState::Running; }
static void Invalidate() { ++invalidations; }
namespace factory_timer {
void ResetHost(bool locked=false) {
    if(ctx.mutex) vSemaphoreDelete(ctx.mutex);
    ctx.~Context();new(&ctx) Context{};
    host.~HostWifi();new(&host) HostWifi{};
    timer=CountdownTimer{};invalidations=0;
    std::strcpy(reinterpret_cast<char*>(host.config.sta.ssid),"FT-ZONE1");
    host.config.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;
    host.config.sta.bssid_set=locked;
    host.current.rssi=-71;host.current.bssid[5]=1;
    std::strcpy(reinterpret_cast<char*>(host.current.ssid),"FT-ZONE1");
    host.disconnected_callback=WifiStationDisconnected;
    host.scan_done_callback=WifiScanCompleted;
    assert(WifiMaintenanceInit({Active,Invalidate})==ESP_OK);
}
void Tick(int64_t now) {
    host.now_us=now;host_tick_waits=0;
    try { MaintenanceTask(nullptr); } catch(const EndHostTick&) {}
    assert(!host_mutex_owned);
}
}
static void Online(bool locked=false) {
    ResetHost(locked);WifiStationStarted();Tick(0);assert(host.connects==1);
    WifiStationConnected();WifiFactoryLinkReady();Tick(0);
}
static wifi_ap_record_t AP(int rssi,int id,const char* ssid="FT-ZONE1",int auth=WIFI_AUTH_WPA2_PSK) {
    wifi_ap_record_t ap{};ap.rssi=static_cast<int8_t>(rssi);ap.bssid[5]=static_cast<uint8_t>(id);
    std::strcpy(reinterpret_cast<char*>(ap.ssid),ssid);ap.authmode=auth;return ap;
}
static void Complete(int64_t now,uint32_t status=0) { WifiScanCompleted(status);Tick(now); }
static void Arm(int64_t now) {
    CommandPacket command{};command.type=CommandType::StartAt;command.command_id=1;
    command.duration_seconds=20;command.start_at_master_us=now+5000000;
    assert(timer.Apply(command,now,now+5000000)==AckResult::Accepted);
}

int main() {
#if !defined(CONFIG_FACTORY_WIFI_IDLE_ROAM) || !CONFIG_FACTORY_WIFI_IDLE_ROAM
    Online();host.current.rssi=-85;
    host.records={AP(-20,2),AP(-10,3,"FT-ZONE2")};
    Tick(60000000);Tick(3600000000LL);
    assert(host.scans==0 && host.disconnects==0 && invalidations==0);
    { WifiControlLease lease(3600000000LL,true);assert(lease.TimingReady());Arm(3600000000LL); }
    timer.Update(3605000000LL,3605000000LL);assert(Active());
    WifiStationDisconnected();Tick(3605000100LL);assert(host.connects==2 && Active());
    WifiStationDisconnected();Tick(3605000200LL);assert(host.connects==3);
    WifiStationDisconnected();Tick(3605000300LL);assert(host.connects==3);
    Tick(3608500299LL);assert(host.connects==3);
    Tick(3608500300LL);assert(host.connects==4 && host.scans==0 && Active());
    assert(std::strcmp(reinterpret_cast<char*>(host.config.sta.ssid),"FT-ZONE1")==0);
    WifiStationConnected();WifiFactoryLinkReady();Tick(3608500400LL);assert(ctx.failures==0);
    std::cout<<"WIFI_ZONE_DEFAULT=PASS no deliberate scans/roams even at -85 dBm; home SSID and active countdown preserved; reconnect backoff/reset retained\n";
    return 0;
#endif
    Online();host.current.rssi=-70;Tick(60000000);assert(host.scans==0);
    host.current.rssi=-71;Tick(120000000);assert(host.scans==1);
    host.records={AP(-62,2)};Complete(120100000);assert(host.disconnects==0 && !host.list_allocated);
    Tick(180100000);host.records={AP(-61,2)};Complete(180200000);
    assert(host.disconnects==1 && invalidations==1 && ctx.roaming);
    { WifiControlLease lease(180200000,true);assert(!lease.TimingReady()); }
    Tick(180300000);assert(host.config.sta.bssid_set && host.config.sta.bssid[5]==2 && host.connects==2);
    WifiStationDisconnected();Tick(180400000);
    assert(!host.config.sta.bssid_set && host.connects==3); // Target failure releases the temporary pin.
    WifiStationConnected();WifiFactoryLinkReady();Tick(180500000);
    assert(ctx.failures==0 && !ctx.roaming);
    std::cout<<"PASS: RSSI/hysteresis boundaries; target pin; failed-target fallback; fresh synchronization barrier\n";

    Online();Tick(60000000);
    host.records={AP(-20,2,"FT-ZONE2"),AP(-30,3,"FT-ZONE1",WIFI_AUTH_OPEN),AP(-61,4)};
    Complete(60100000);Tick(60200000);
    assert(host.config.sta.bssid[5]==4);
    assert(std::strcmp(reinterpret_cast<char*>(host.config.sta.ssid),"FT-ZONE1")==0);
    Online(true);Tick(1000000000);assert(host.scans==0);
    std::cout<<"PASS: cross-zone and weaker-security APs rejected; permanent BSSID lock suppresses roaming\n";

    Online();Tick(60000000);
    { WifiControlLease lease(60000001,true);assert(!lease.TimingReady()); }
    Tick(60000002);assert(ctx.scanning && host.stops==1);
    Tick(60000003);assert(!ctx.scanning && !host.list_allocated);
    Complete(60000003);assert(host.disconnects==0);
    { WifiControlLease lease(60000004,true);assert(lease.TimingReady());Arm(60000004); }
    Tick(200000000);assert(host.scans==1);
    std::cout<<"PASS: scan cancellation before timing preparation; late completion cannot roam; ARMED blocks scans\n";

    Online();Tick(60000000);host.stop_result=ESP_FAIL;
    { WifiControlLease lease(60000001,true);assert(!lease.TimingReady()); }
    Tick(60000002);assert(ctx.scanning);
    { WifiControlLease lease(60000003,true);assert(!lease.TimingReady()); }
    Complete(60000004,1);assert(!ctx.scanning && !host.list_allocated);
    { WifiControlLease lease(60000005,true);assert(lease.TimingReady()); }
    Online();Tick(60000000);Tick(65000000);assert(host.stops==1 && ctx.scanning);
    Tick(65000001);assert(!ctx.scanning);
    Online();Tick(60000000);host.records_result=ESP_FAIL;Complete(60000001);assert(!host.list_allocated);
    std::cout<<"PASS: failed scan stop keeps START barred; completion/watchdog and error cleanup release records\n";
    Online();Tick(60000000);host.auto_scan_stop_done=false;
    { WifiControlLease lease(60000001,true);assert(!lease.TimingReady()); }
    Tick(60000002);assert(ctx.scanning && ctx.scan_stop_requested);
    { WifiControlLease lease(60000003,true);assert(!lease.TimingReady()); }
    Complete(60000004,0);assert(!ctx.scanning && host.disconnects==0 && !host.list_allocated);
    { WifiControlLease lease(60000005,true);assert(lease.TimingReady()); }
    std::cout<<"PASS: successful scan-stop return is insufficient; delayed SCAN_DONE is the START barrier\n";

    Online();
    std::thread worker;
    std::promise<void> attempted;
    {
        WifiControlLease lease(60000000,true);assert(lease.TimingReady());
        worker=std::thread([&] {
            bool acquired=ctx.mutex->try_lock();if(acquired) ctx.mutex->unlock();
            assert(!acquired);attempted.set_value();Tick(60000000);
        });
        attempted.get_future().wait();Arm(60000000);
    }
    worker.join();assert(host.scans==0 && timer.Snapshot().state==TimerState::Armed);
    timer.Update(65000000,65000000);Tick(65000000);assert(timer.Snapshot().state==TimerState::Running);
    Tick(84000000);assert(host.scans==0);
    timer.Update(85000000,85000000);Tick(85000000);Tick(114999999);assert(host.scans==0);
    Tick(115000000);assert(host.scans==1);
    std::cout<<"PASS: real task/control mutex interleaving; actual countdown ARMED/RUNNING and 30s post-run holdoff\n";

    Online();
    for(int n=1;n<=3;++n) { WifiStationDisconnected();Tick(n*100);assert(host.connects==static_cast<unsigned>(n<3?n+1:3)); }
    const int64_t due=300+3500000;Tick(due-1);assert(host.connects==3);Tick(due);assert(host.connects==4);
    WifiStationConnected();Tick(due+10000000);assert(host.connects==4); // DHCP is not a failed Wi-Fi association.
    WifiFactoryLinkReady();Tick(due+10000001);assert(ctx.failures==0);
    WifiStationDisconnected();Tick(due+10000002);assert(host.connects==5);
    ResetHost();host.connect_result=ESP_FAIL;WifiStationStarted();Tick(0);Tick(1);Tick(2);
    assert(host.connects==3);Tick(3500001);assert(host.connects==3);Tick(3500002);assert(host.connects==4);
    std::cout<<"PASS: first retries prompt; 3.5s jittered backoff; accepted-link reset; synchronous API failures bounded\n";

    Online();{ WifiControlLease lease(0,false);assert(lease.TimingReady()); }
    Tick(60000000);assert(host.scans==1); // Normal STATUS does not postpone roaming forever.
    host.current.rssi=-60;Complete(60000001);assert(host.disconnects==0);
    Online();{ WifiControlLease lease(50000000,true);assert(lease.TimingReady()); }
    Tick(79999999);assert(host.scans==0);Tick(80000000);assert(host.scans==1);
    Online();{ WifiControlLease lease(0,true);Arm(0); }
    timer.Update(5000000,5000000);WifiStationDisconnected();Tick(5000000);assert(host.connects==2 && Active());
    std::cout<<"PASS: STATUS permits maintenance; timing quiet window; real link-loss recovery continues during countdown\n";
    Online();Tick(60000000);host.records={AP(-61,2)};Complete(60000001);
    host.config_result=ESP_FAIL;Tick(60000002);assert(!ctx.target_pending && ctx.pin_in_use);
    host.config_result=ESP_OK;Tick(60000003);assert(!host.config.sta.bssid_set);
    Online();host.scan_result=ESP_FAIL;Tick(60000000);Tick(60000001);assert(host.scans==1 && !ctx.scanning);
    ResetHost();WifiStationStarted();Tick(0);WifiStationConnected();Tick(120000000);
    assert(host.connects==1 && host.scans==0);
    { WifiControlLease lease(120000001,true);assert(!lease.TimingReady()); }
    std::cout<<"PASS: configuration/scan errors recover; association without an accepted LAN lease cannot arm\n";
    std::cout<<"WIFI_MAINTENANCE_HOST=PASS (actual task/driver adapter with host radio APIs)\n";
}
