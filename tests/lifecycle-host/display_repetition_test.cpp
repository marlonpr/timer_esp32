#include <cassert>
#include <iostream>
#include <array>
#include <vector>
#include <algorithm>
#include <tuple>
#define FACTORY_INVENTORY_HOST_SPY 1
#define FACTORY_DISPLAY_HOST_HOOKS 1
#ifndef FACTORY_DISPLAY_SOURCE_PATH
#define FACTORY_DISPLAY_SOURCE_PATH "../../../components/factory_display/factory_display.cpp"
#endif
#include FACTORY_DISPLAY_SOURCE_PATH
#include "countdown_timer.h"
#include "start_task_test_delay.h"

int64_t host_now_us=1000000;
uint64_t host_gptimer_count=0,host_next_alarm=0;
char host_output[2000000];
size_t host_output_used=0;
TaskHandle_t host_current_task=nullptr;
unsigned host_inventory_calls=0;
bool host_monitoring=false;

const uint32_t kIdleLogoBitmap[kIdleLogoWidth*kIdleLogoHeight]={};
extern "C" bool factory_display_backend_init(uint8_t){return true;}
extern "C" void factory_display_backend_set_brightness_percent(uint8_t){}
using Frame=std::array<uint32_t,64*32>;
Frame back{};
std::vector<Frame> frames;
std::vector<int64_t> targets;
extern "C" void factory_display_backend_clear(){back.fill(0);}
extern "C" void factory_display_backend_set_pixel(int x,int y,uint8_t r,uint8_t g,uint8_t b){
    if(x>=0&&x<64&&y>=0&&y<32)back[y*64+x]=(uint32_t(r)<<16)|(uint32_t(g)<<8)|b;
}
extern "C" void factory_display_backend_fill_rect(int x,int y,int w,int h,uint8_t r,uint8_t g,uint8_t b){
    for(int row=y;row<y+h;++row)for(int col=x;col<x+w;++col)factory_display_backend_set_pixel(col,row,r,g,b);
}
extern "C" void factory_display_backend_flip(){}
extern "C" void factory_display_backend_publish_prepared_from_isr(){frames.push_back(back);targets.push_back(host_now_us);}
// Clock domains are deliberately different: an accidental local/discipline
// substitution must also fail the exact armed-epoch checks.
constexpr int64_t kClockOffset=8000000;
extern "C" int64_t rtc_discipline_local_to_disciplined_us(int64_t local){return local+kClockOffset;}
extern "C" int64_t rtc_discipline_disciplined_to_local_us(int64_t disciplined){return disciplined-kClockOffset;}
bool cpu0_latency_monitor_init(){return true;}
void cpu0_latency_monitor_cache_task_inventory(){}
void cpu0_latency_monitor_prepare_run(uint64_t){}
void cpu0_latency_monitor_begin_run(uint64_t,int64_t){}
void cpu0_latency_monitor_end_run(uint64_t){}
void cpu0_latency_monitor_dump_run(){}
void cpu0_latency_monitor_set_commit_target(uint32_t,int64_t){}
void cpu0_latency_monitor_note_commit(uint32_t,int64_t,int64_t,int64_t,int64_t){}
void cpu0_latency_monitor_get_summary(cpu0_latency_monitor_summary_t* out){*out={};}

struct RunComplete {};
DisplayCommand pending{};
bool queued=false,publication_active=false,started=false;
int64_t publication_target=0,actual_start=0;
factory_timer::CountdownTimer countdown(30);

int host_queue_receive(void* out,uint32_t timeout) {
    if(queued){*static_cast<DisplayCommand*>(out)=pending;queued=false;return pdTRUE;}
    if(timeout==portMAX_DELAY)throw RunComplete{};
    return pdFALSE;
}
int host_queue_overwrite(const void* value){pending=*static_cast<const DisplayCommand*>(value);queued=true;return pdTRUE;}
int host_start_publication(uint64_t delay){publication_target=host_now_us+static_cast<int64_t>(delay);publication_active=true;return ESP_OK;}
int host_stop_publication(){publication_active=false;return ESP_OK;}
bool host_publication_active(){return publication_active;}
unsigned host_wait_for_publication() {
    assert(publication_active && publication_target>=host_now_us);
    host_now_us=publication_target;
    publication_active=false;
    // The independent COMMIT ISR runs while the high-priority START task is
    // intentionally delayed. Then let the actual display task continue late.
    BoundaryPublishTimerCallback(nullptr);
    if(!started){
#if defined(CONFIG_FACTORY_START_TASK_TEST_DELAY_US) && CONFIG_FACTORY_START_TASK_TEST_DELAY_US > 0
        host_now_us=factory_timer::InjectStartTaskTestDelay();
#endif
        const auto result=countdown.Update(host_now_us,rtc_discipline_local_to_disciplined_us(host_now_us));
        assert(result.state==factory_timer::TimerState::Running);
        actual_start=host_now_us;
        started=true;
    }
    return 1;
}

int main() {
    s_ready.store(true);s_command_queue=(void*)1;s_display_task=(void*)1;s_boundary_publish_timer=(void*)1;
    factory_display_cache_runtime_inventory();host_monitoring=true;
    const std::tuple<uint32_t,uint32_t,uint32_t> cases[]={{20,1,0},{20,3,0},{5,30,0},{3000,2,0},{5999,1,0},{180,3,0},{1,100,0},{20,3,4},{5,30,4},{1,3,4},{5999,1,4},{180,3,4}};
    uint64_t command=0;
    for(auto [duration,repeats,logo]:cases){
        ++command;
        frames.clear();targets.clear();
        std::vector<Frame> expected;
        const uint32_t seconds=(duration+logo)*repeats;
        for(uint32_t n=0;n<=seconds;++n){
            const uint32_t phase=n%(duration+logo);
            if(logo && (phase>=duration || n==seconds))RenderIdleToBackBuffer();
            else if(n==seconds)RenderFinishedToBackBuffer();
            else RenderRunningToBackBuffer(duration-phase);
            expected.push_back(back);
        }
        const int64_t local_tstar=host_now_us+5000000;
        const int64_t disciplined_tstar=rtc_discipline_local_to_disciplined_us(local_tstar);
        factory_timer::CommandPacket arm{};
        arm.type=factory_timer::CommandType::StartAt;arm.command_id=command;arm.duration_seconds=duration;
        arm.repeat_count=repeats;arm.total_repeats=repeats;arm.logo_seconds=logo;
        assert(countdown.Apply(arm,host_now_us,local_tstar)==factory_timer::AckResult::Accepted);
        countdown.SetScheduledRunningMicroseconds(disciplined_tstar);
        started=false;
        factory_display_begin_run_monitor(command,local_tstar);
        factory_display_arm_program(local_tstar,disciplined_tstar,duration,repeats,logo,command);
        try{DisplayTask(nullptr);}catch(const RunComplete&){}
        assert(started && frames.size()==seconds+1 && frames==expected);
        assert(actual_start-local_tstar==CONFIG_FACTORY_START_TASK_TEST_DELAY_US);
        assert(countdown.StartRunningMicroseconds()==disciplined_tstar);
        for(size_t n=0;n<targets.size();++n)assert(targets[n]==local_tstar+int64_t(n)*1000000);
        auto final=countdown.Update(host_now_us,rtc_discipline_local_to_disciplined_us(host_now_us));
        assert(final.state==factory_timer::TimerState::Finished && final.remaining_seconds==0);
        assert(s_frame_not_ready_count.load()==0 && host_inventory_calls==1);
        std::cout<<"PASS: actual display "<<duration<<" s x "<<repeats<<" logo="<<logo<<" frames="<<frames.size()
                 <<" START task delay="<<actual_start-local_tstar<<" us; no internal zero frame or extra second\n";
    }
}

namespace factory_output {
bool Init(){return true;} bool Ready(){return true;}
void Schedule(int64_t,uint32_t,uint32_t,uint32_t){} void Cancel(){}
}
