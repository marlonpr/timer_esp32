#include <cassert>
#include <iostream>
#include "../../../components/factory_display/cpu0_latency_monitor.cpp"
int64_t host_now_us=1000000;
uint64_t host_gptimer_count=0,host_next_alarm=0;
char host_output[2000000];
size_t host_output_used=0;
TaskHandle_t host_current_task=nullptr;

int main() {
    assert(cpu0_latency_monitor_init());
    cpu0_latency_monitor_cache_task_inventory();
    cpu0_latency_monitor_prepare_run(1);
    host_gptimer_count=123;
    cpu0_latency_monitor_begin_run(1,host_now_us+5000000);
#if defined(CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY) && CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY
    assert(s_isr_canary.requested_after_begin_us==5000000+kIsrCanaryOffsetUs);
#endif
    const uint64_t start=host_gptimer_count;
    const int64_t local_start=host_now_us;
    uint32_t rtc=0,wifi=0,udp=0;
    unsigned calls=0;
    // Include the full five-second ARMED window and 30-second countdown.
    while(host_next_alarm < start+35000000) {
        const uint64_t alarm=host_next_alarm;
        unsigned late=0;
        if(calls<90) {late=60;host_current_task=(void*)1;++rtc;}
        else if(calls<170) {late=70;host_current_task=(void*)2;++wifi;}
        else if(calls<240) {late=80;host_current_task=(void*)3;++udp;}
        if(calls==1000) {late=1000;host_current_task=(void*)2;++wifi;}
        host_gptimer_count=alarm+late;
        host_now_us=local_start+(int64_t)(host_gptimer_count-start);
        gptimer_alarm_event_data_t e{host_gptimer_count,alarm};
        OnAlarm(s_timer,&e,nullptr);
        ++calls;
    }
    cpu0_latency_monitor_note_commit(0,10000000,10000010,10000060,10000062);
    cpu0_latency_monitor_note_commit(1,11000000,11000010,11000299,11000300);
    cpu0_latency_monitor_note_commit(2,12000000,12000010,12000300,12000301);
    host_gptimer_count=start+35000000;
    host_now_us=local_start+35000000;
    cpu0_latency_monitor_end_run(1);
    cpu0_latency_monitor_summary_t m{};cpu0_latency_monitor_get_summary(&m);
    assert(m.valid && m.monitor_start_us==local_start);
    assert(m.tstar_local_us-m.monitor_start_us==5000000);
    assert(m.monitor_end_us-m.monitor_start_us==35000000);
    assert(m.first_alarm_offset_us==128);
    assert(m.expected_periods==host_gptimer_count/251-start/251);
    assert((uint64_t)m.sample_callbacks+m.missed_periods==m.expected_periods);
    // The 1000 us blocker swallows three deadlines and ends only 4 us before
    // the fourth. v6.23.17 never programs a deadline that close, so it skips
    // that one too and charges it as missed (3 + 1).
    assert(m.missed_periods==4);
    assert(m.rearm_guard_skips==1);
    assert(m.rtc_discipline_events_ge_50us==rtc);
    assert(m.wifi_events_ge_50us==wifi);
    assert(m.udp_events_ge_50us==udp);
    assert(m.event_count==rtc+wifi+udp && m.event_count>64);
    assert(m.event_overflow==m.event_count-64);
    assert(m.commit_late_count==3 && m.commit_ge_300us==1);
    assert(m.worst_commit_lateness_us==300);
    // A stale display completion must not stop a newly accepted command.
    cpu0_latency_monitor_prepare_run(2);
    cpu0_latency_monitor_begin_run(2,host_now_us+5000000);
    cpu0_latency_monitor_end_run(1);
    assert(s_run_active);
    cpu0_latency_monitor_end_run(2);
    // A pending alarm from before acceptance has no eligible period yet.
    cpu0_latency_monitor_prepare_run(3);
    host_gptimer_count=5000;
    cpu0_latency_monitor_begin_run(3,host_now_us+5000000);
    gptimer_alarm_event_data_t e{5001,4769};
    OnAlarm(s_timer,&e,nullptr);
    assert(s_sample_callbacks==0 && s_missed_periods==0);
    std::cout<<"PASS: actual monitor source; ARMED coverage, grid endpoints, missed periods, full counters and actual COMMIT gate\n";
}
