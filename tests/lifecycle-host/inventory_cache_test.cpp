#include <cassert>
#include <iostream>
#define FACTORY_INVENTORY_HOST_SPY 1
#include "../../../components/factory_display/cpu0_latency_monitor.cpp"
int64_t host_now_us=1000000;
uint64_t host_gptimer_count=0,host_next_alarm=0;
char host_output[2000000];
size_t host_output_used=0;
TaskHandle_t host_current_task=nullptr;
unsigned host_inventory_calls=0;
bool host_monitoring=false;

int main() {
    assert(cpu0_latency_monitor_init());
    cpu0_latency_monitor_cache_task_inventory();
    cpu0_latency_monitor_cache_task_inventory();
    assert(host_inventory_calls==1 && s_task_inventory_count==3);
    assert(s_rtc_task==(void*)1 && s_wifi_task==(void*)2 && s_udp_task==(void*)3);
    // No full task scan is permitted after START_AT can be accepted, including
    // prepare/begin/end, repeated runs, and post-run task-name resolution.
    host_monitoring=true;
    for(uint64_t command=1;command<=3;++command) {
        cpu0_latency_monitor_prepare_run(command);
        cpu0_latency_monitor_begin_run(command,host_now_us+5000000);
        cpu0_latency_monitor_cache_task_inventory();
        host_current_task=(void*)2;
        host_gptimer_count=host_next_alarm+400;
        host_now_us+=400;
        const gptimer_alarm_event_data_t e{host_gptimer_count,host_next_alarm};
        OnAlarm(s_timer,&e,nullptr);
        cpu0_latency_monitor_end_run(command);
        cpu0_latency_monitor_dump_run();
        cpu0_latency_monitor_summary_t summary{};
        cpu0_latency_monitor_get_summary(&summary);
        assert(std::strcmp(summary.worst_task,"wifi")==0);
    }
    assert(host_inventory_calls==1);
    std::cout<<"PASS: monitor inventory scanned once before acceptance; cached names/counters survive repeated runs\n";
}
