#include <cassert>
#include <iostream>
#include <vector>
#define FACTORY_OUTPUT_HOST_HOOKS 1
#include "../../components/factory_display/repetition_output.cpp"

int64_t host_now_us=1000000;
uint64_t host_gptimer_count=0,host_next_alarm=0;
char host_output[2000000];size_t host_output_used=0;
TaskHandle_t host_current_task=nullptr;
bool high=false,armed=false,fail_arm=false,configured=false;
int64_t deadline=0;
std::vector<std::pair<int64_t,bool>> edges;
extern "C" int64_t rtc_discipline_local_to_disciplined_us(int64_t local){return local+8000000;}
extern "C" int64_t rtc_discipline_disciplined_to_local_us(int64_t disciplined){return disciplined-8000000;}
int host_output_start_timer(uint64_t delay){if(fail_arm)return ESP_FAIL;deadline=host_now_us+delay;armed=true;return ESP_OK;}
int host_output_stop_timer(){armed=false;return ESP_OK;}
void host_output_gpio_write(int pin,uint32_t level){assert(pin==33);if(high!=(level!=0)){high=level!=0;edges.push_back({host_now_us,high});}}
void host_output_gpio_configure(uint64_t mask){assert(mask==(1ULL<<33) && high);configured=true;}
void Fire(int64_t now){host_now_us=now;factory_output::OnTimer(nullptr);}

int main(){
    assert(factory_output::Init() && factory_output::Ready() && configured && high);
    edges.clear(); // OFF was preloaded before GPIO output was enabled.
    factory_output::Cancel();assert(high && !armed);
    factory_output::Schedule(14000000,20,3,0);assert(high && !armed); // legacy run stays OFF
    factory_output::Schedule(14000000,20,3,4); // local T*=6s, acceptance=1s
    assert(!high && deadline==5000000);
    Fire(4999999);assert(!high && deadline==5000000);
    Fire(5000000);assert(high && deadline==26000000);
    for(int repetition=0;repetition<3;repetition++){
        const int64_t begin=26000000+int64_t(repetition)*24000000;
        for(int half=0;half<8;half++){
            Fire(begin+int64_t(half)*500000);
            assert(high==(half%2!=0));
            assert(deadline==begin+int64_t(half+1)*500000);
        }
        Fire(begin+4000000);assert(high);
    }
    assert(!armed && edges.size()==26); // initial fall/rise plus 3*8 blink edges
    host_now_us=100000000;factory_output::Schedule(113000000,20,3,4);
    assert(!high);factory_output::Cancel();assert(high && !armed);
    Fire(104000000);assert(high && !armed); // callback queued before RESET
    host_now_us=110000000;factory_output::Schedule(123000000,20,3,4);
    Fire(114000000);assert(high && deadline==135000000);
    Fire(135750000);assert(high && deadline==136000000); // coalesce a late callback
    Fire(136000000);assert(!high && deadline==136500000); // absolute grid is retained
    host_now_us=136200000;factory_output::Schedule(149200000,5,1,4);
    assert(!high && deadline==140200000); // replacement restarts intro exactly once
    factory_output::Cancel();fail_arm=true;
    factory_output::Schedule(149200000,5,1,4);assert(high && !armed);
    fail_arm=false;factory_output::Schedule(149200000,5,1,4);
    Fire(140200000);assert(high && deadline==146200000);
    fail_arm=true;Fire(146200000);assert(high && !armed); // blink re-arm failure returns OFF
    std::cout<<"GPIO33_OUTPUT=PASS active_low=1 boot_high=1 intro=4000000us blink=500000us four_cycles_each repetition_gap=4s cancel_high=1 legacy_high=1 absolute_phase=1 fail_high=1\n";
}
