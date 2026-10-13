#include "countdown_timer.h"
#include "network_policy.h"
#include "segment_schedule.h"
#include <cassert>
#include <climits>
#include <cstring>
#include <iostream>
#include <string>
using namespace factory_timer;
CommandPacket Segment(uint32_t d, uint32_t n, uint32_t first=1, uint32_t total=30) {
    CommandPacket c{};
    c.type=CommandType::StartAt;c.command_id=11;c.duration_seconds=d;c.start_at_master_us=9000000;
    c.plan_id=0x123;c.segment_index=1;c.repeat_count=n;c.first_repeat=first;c.total_repeats=total;
    c.expected_boot_id=0x456;
    return c;
}
void Schedule(uint32_t d,uint32_t n,uint32_t first,uint32_t total) {
    CountdownTimer t;
    auto c=Segment(d,n,first,total);
    assert(t.Apply(c,1000000,6000000)==AckResult::Accepted);
    constexpr int64_t epoch=14000000;
    t.SetScheduledRunningMicroseconds(epoch);
    auto s=t.Update(6000500,epoch+500);
    assert(t.StartRunningMicroseconds()==epoch && t.EndRunningMicroseconds()==epoch+int64_t(d)*n*1000000);
    assert(s.elapsed_segment_microseconds==500 && s.state==TimerState::Running);
    for(uint32_t second=0;second<=d*n;++second){
        s=t.Update(6000000+int64_t(second)*1000000,epoch+int64_t(second)*1000000);
        assert(s.elapsed_segment_seconds==second);
        assert(s.remaining_seconds==(second==d*n ? 0 : d-second%d));
        assert(s.current_repeat==first+std::min(second/d,n-1));
        assert(s.state==(second==d*n ? TimerState::Finished : TimerState::Running));
    }
    assert(t.Apply(c,INT64_MAX,6000000)==AckResult::Duplicate);
    auto changed=c;changed.repeat_count=1;changed.total_repeats=1;
    if(n==1) changed.start_at_master_us++;
    assert(t.Apply(changed,1,6000000)==AckResult::Invalid);
}
int main(){
    Schedule(20,1,1,1);Schedule(20,3,1,3);Schedule(5,30,1,30);
    Schedule(3000,2,1,2);Schedule(5999,1,1,2);Schedule(5999,1,2,2);
    Schedule(180,3,1,10);Schedule(180,3,4,10);Schedule(180,3,7,10);Schedule(180,1,10,10);
    assert(!ValidSegment(5999,2,1,2,1));assert(!ValidSegment(0,1,1,1,1));
    assert(!ValidSegment(5,3,UINT32_MAX,UINT32_MAX,1));
    CountdownTimer t;auto c=Segment(5,30);
    assert(t.Apply(c,1,6000000)==AckResult::Accepted);
    auto rival=c;rival.command_id++;rival.expected_previous_command_id=11;
    assert(t.Apply(rival,1,6000000)==AckResult::Busy);
    CommandPacket reset{};reset.type=CommandType::Reset;reset.command_id=12;reset.duration_seconds=5;
    assert(t.Apply(reset,1)==AckResult::Accepted);
    rival.command_id=13;rival.expected_previous_command_id=12;
    assert(t.Apply(rival,1,6000000)==AckResult::Accepted); // cancelled ARM may be retried
    t.SetScheduledRunningMicroseconds(6000000);t.Update(156000000);
    auto next=rival;next.segment_index=2;next.first_repeat=31;next.total_repeats=60;next.command_id=14;next.expected_previous_command_id=13;
    assert(t.Apply(next,160000000,165000000)==AckResult::Accepted);
    reset.command_id=15;assert(t.Apply(reset,160000000)==AckResult::Accepted);
    // An expired command cannot restart an older plan after its ID leaves the cache.
    for(uint64_t id=16;id<40;++id){reset.command_id=id;assert(t.Apply(reset,160000000)==AckResult::Accepted);}
    c.expected_previous_command_id=0;
    assert(t.Apply(c,160000000,165000000)==AckResult::Invalid);
    RtcRecoveryWindow r;
    assert(r.Observe(1000,0,0,0,true)==129);
    assert(r.Observe(1001,0,0,1,true)==0);
    assert(r.Observe(1129,0,0,1,true)==128);
    assert(r.Observe(1130,0,0,1,true)==129);
    assert(r.Observe(1131,0,0,1,false)==0);
    assert(r.Observe(1140,0,0,1,false)==9);
    assert(r.Observe(1260,0,0,1,true)==129);
    assert(r.Observe(1,0,0,0,true)==0);
    RtcRecoveryWindow unknown;
    assert(unknown.Observe(1000,1,0,0,true)==0);
    assert(unknown.Observe(1129,1,0,0,true)==129);
    CommandPacket parsed{};ParseError error{};
    const std::string wire="FCT2|CMD|START_AT|000000000000000B|5|9000000|0000000000000123|1|30|1|30|0000000000000456|0000000000000000";
    assert(ParseCommand(wire,parsed,error));assert(parsed.repeat_count==30 && parsed.expected_boot_id==0x456);
    assert(!ParseCommand(wire+"|0",parsed,error));
    assert(!ParseCommand("FCT2|CMD|START_AT|000000000000000B|5999|9000000|0000000000000123|1|2|1|2|0000000000000456|0000000000000000",parsed,error));
    const std::string maximum_command="FCT2|CMD|START_AT|FFFFFFFFFFFFFFFF|1|9223372036854775807|FFFFFFFFFFFFFFFF|4294967295|6000|4294961296|4294967295|FFFFFFFFFFFFFFFF|FFFFFFFFFFFFFFFF";
    assert(ParseCommand(maximum_command,parsed,error));
    assert(maximum_command.size()<=kMaxPacketLength);
    std::cout<<"SEGMENT_COMMAND_MAX_WIDTH_BYTES="<<maximum_command.size()<<'\n';
    char buffer[kMaxPacketLength+1];
    TimerSnapshot maximum{TimerState::Finished,UINT32_MAX,UINT32_MAX,UINT64_MAX,
        UINT64_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,INT64_MAX,INT64_MAX};
    int length=FormatExecutionStatus(buffer,sizeof(buffer),"ESP15",UINT64_MAX,UINT64_MAX,maximum,INT64_MAX,UINT16_MAX,INT64_MIN);
    assert(length>0 && length<=int(kMaxPacketLength) && std::strlen(buffer)==size_t(length));
    std::cout<<"EXEC_STATUS_MAX_WIDTH_BYTES="<<length<<'\n';
    TimerSnapshot fixture{TimerState::Running,5,5,11,0x123,1,30,1,30,2,5,9000000,5000500};
    FormatExecutionStatus(buffer,sizeof(buffer),"ESP01",0x789,0x456,fixture,6000000,129,5000500);
    std::cout<<"EXEC_STATUS_FIXTURE="<<buffer<<'\n';
    length=FormatSyncReply(buffer,sizeof(buffer),"ESP15",UINT64_MAX,INT64_MAX,INT64_MAX,INT64_MAX,INT64_MAX,UINT32_MAX,true,INT32_MAX,"FE:FF:FF:FF:FF:FF");
    assert(length>0 && length<=int(kMaxPacketLength));
    std::array<uint8_t,6> bssid{};
    assert(ParseBssid("02:12:34:56:78:Ab",bssid)&&bssid[5]==0xAB);
    for(auto bad:{"00:00:00:00:00:00","FF:FF:FF:FF:FF:FF","02-12:34:56:78:AB","02:12:34:56:78:AG","02:12:34:56:78:A"}) assert(!ParseBssid(bad,bssid));
    std::cout<<"PASS: segmented schedules, +500 us late START, duplicate/stale command fences, recovery counters, packet widths and optional BSSID\n";
}
