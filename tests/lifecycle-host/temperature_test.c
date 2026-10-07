#include <assert.h>
#include "../../../components/rtc_discipline/rtc_discipline.c"
int64_t host_now_us=1000000;
uint64_t host_gptimer_count=0,host_next_alarm=0;
char host_output[2000000];size_t host_output_used=0;
TaskHandle_t host_current_task=NULL;
static unsigned reads=0;
static esp_err_t read_result=ESP_OK;
esp_err_t ds3231_get_temperature_c(ds3231_dev_t* dev,float* out) {
    (void)dev;++reads;host_now_us+=110;*out=26.25f;return read_result;
}
int main(void) {
    ds3231_dev_t rtc={0};s_ctx.rtc=&rtc;s_ctx.initialized=true;
    s_ctx.temperature_mutex=xSemaphoreCreateMutex();
    refresh_temperature();assert(reads==1 && s_ctx.status.rtc_temperature_valid);
    rtc_discipline_begin_run(10);
    for(unsigned i=0;i<40;++i) refresh_temperature();
    assert(reads==1); // no periodic transaction during ARMED or RUNNING
    const int64_t finished=host_now_us;
    assert(rtc_discipline_finish_run_temperature(10,finished));
    assert(reads==2 && s_ctx.status.rtc_temperature_sample_local_us>finished);
    assert(s_ctx.post_run_temperature_ready);
    assert(strstr(host_output,"RTC_TEMP_POST_RUN command=000000000000000A"));
    assert(strstr(host_output,"periodic_reads_during_run=0 suppressed_refreshes=40"));
    assert(rtc_discipline_finish_run_temperature(10,finished));assert(reads==2);
    refresh_temperature();assert(reads==3);
    rtc_discipline_begin_run(11);read_result=ESP_FAIL;
    assert(!rtc_discipline_finish_run_temperature(11,host_now_us));
    assert(!s_ctx.post_run_temperature_ready && !s_ctx.status.rtc_temperature_valid);
    unsigned after_failure=reads;refresh_temperature();assert(reads==after_failure);
    read_result=ESP_OK;
    assert(rtc_discipline_finish_run_temperature(11,host_now_us));
    /* Tripwire: if suppression were ever lifted mid-run, the log must say so. */
    rtc_discipline_begin_run(13);s_ctx.temperature_suppressed=false;
    refresh_temperature();assert(s_ctx.run_periodic_reads==1);
    s_ctx.temperature_suppressed=true;host_output_used=0;host_output[0]='\0';
    assert(rtc_discipline_finish_run_temperature(13,host_now_us));
    assert(strstr(host_output,"periodic_reads_during_run=1 suppressed_refreshes=0"));
    rtc_discipline_begin_run(12);rtc_discipline_cancel_run();
    refresh_temperature();assert(!s_ctx.temperature_suppressed);
    assert(!rtc_discipline_finish_run_temperature(12,host_now_us));
    puts("PASS: actual RTC source; suppression, immediate one-shot, timestamp, freshness readiness, failure retry and RESET");
}
