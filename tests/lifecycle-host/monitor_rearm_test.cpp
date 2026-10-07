#include <cassert>
#include <iostream>
#include "../../../components/factory_display/cpu0_latency_monitor.cpp"
int64_t host_now_us=1000000;
uint64_t host_gptimer_count=0,host_next_alarm=0;
char host_output[2000000];
size_t host_output_used=0;
TaskHandle_t host_current_task=nullptr;

// v6.23.17: drive the real OnAlarm() as an equality-only comparator would
// behave (a deadline written at or before the current count never fires) and
// model ISR work between the driver's count capture and the re-arm write.
// Blockers that end just before the next grid deadline must not stop the
// sampler, and every grid deadline must still be counted exactly once.
int main() {
    assert(cpu0_latency_monitor_init());
    cpu0_latency_monitor_cache_task_inventory();
    cpu0_latency_monitor_prepare_run(7);
    host_gptimer_count=0;
    cpu0_latency_monitor_begin_run(7,host_now_us+5000000);
    const uint64_t start=host_gptimer_count;
    const int64_t local_start=host_now_us;
    unsigned calls=0,near_edge=0;
    while(host_next_alarm<start+2000000) {
        const uint64_t alarm=host_next_alarm;
        uint64_t late=3;
        const uint64_t isr_work=4;
        if(calls%500==250) {late=251-2;++near_edge;}      // ends 2 us before next deadline
        if(calls%500==400) {late=2*251-6;++near_edge;}    // two periods, 6 us margin
        host_gptimer_count=alarm+late;
        host_now_us=local_start+(int64_t)(host_gptimer_count-start);
        gptimer_alarm_event_data_t e{host_gptimer_count,alarm};
        host_gptimer_count+=isr_work;                     // time consumed before re-arm
        OnAlarm(s_timer,&e,nullptr);
        assert(host_next_alarm>host_gptimer_count);       // never armed in the past
        assert(host_next_alarm%251==0);                   // absolute grid preserved
        assert(host_next_alarm-host_gptimer_count>=kRearmMinLeadUs);
        ++calls;
    }
    host_gptimer_count=host_next_alarm-1;
    host_now_us=local_start+(int64_t)(host_gptimer_count-start);
    cpu0_latency_monitor_end_run(7);
    cpu0_latency_monitor_summary_t m{};cpu0_latency_monitor_get_summary(&m);
    assert(m.valid && m.rearm_failures==0);
    assert(m.rearm_guard_skips==near_edge);
    assert((uint64_t)m.sample_callbacks+m.missed_periods==m.expected_periods);
    // 249 us late: 0 swallowed + 1 guard skip; 496 us late: 1 swallowed + 1 guard skip.
    assert(m.missed_periods==near_edge+near_edge/2);
    std::cout<<"PASS: re-arm guard; "<<near_edge<<" near-edge blockers, sampler never armed in the past, "
             <<m.sample_callbacks<<" callbacks + "<<m.missed_periods<<" missed = "<<m.expected_periods<<" deadlines\n";
}
