from pathlib import Path
import sys
root=Path(__file__).resolve().parents[1]
mon=(root/'components/factory_display/cpu0_latency_monitor.cpp').read_text()
disp=(root/'components/factory_display/factory_display.cpp').read_text()
kcfg=(root/'main/Kconfig.projbuild').read_text()
e1=(root/'sdkconfig.esp01').read_text(); e2=(root/'sdkconfig.esp02').read_text()
checks={
'historical heartbeats suppressed esp01':'CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT=y' in e1,
'historical heartbeats suppressed esp02':'CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT=y' in e2,
'presentation offset zero esp01':'CONFIG_FACTORY_ANALYZER_PRESENTATION_OFFSET_US=0' in e1,
'presentation offset zero esp02':'CONFIG_FACTORY_ANALYZER_PRESENTATION_OFFSET_US=0' in e2,
'presentation offset compile guard':'v6.23.10 Stage B forbids analyzer presentation offset' in disp,
'sampler enabled both':'CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y' in e1 and 'CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y' in e2,
'250us period both':'CONFIG_FACTORY_CPU0_LATENCY_PERIOD_US=250' in e1 and 'CONFIG_FACTORY_CPU0_LATENCY_PERIOD_US=250' in e2,
'50us threshold both':'CONFIG_FACTORY_CPU0_LATENCY_THRESHOLD_US=50' in e1 and 'CONFIG_FACTORY_CPU0_LATENCY_THRESHOLD_US=50' in e2,
'GPIO32 both':'CONFIG_FACTORY_CPU0_LATENCY_PROBE_GPIO=32' in e1 and 'CONFIG_FACTORY_CPU0_LATENCY_PROBE_GPIO=32' in e2,
'task canary 600us':'CONFIG_FACTORY_CPU0_LATENCY_CANARY_HOLD_US=600' in e1 and 'CONFIG_FACTORY_CPU0_LATENCY_CANARY_HOLD_US=600' in e2,
'isr canary 500us':'CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY_HOLD_US=500' in e1 and 'CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY_HOLD_US=500' in e2,
'Kconfig ISR canary':'config FACTORY_CPU0_LATENCY_ISR_CANARY_HOLD_US' in kcfg,
'commit level from sdkconfig':'kCommitInterruptLevel = CONFIG_ESP_TIMER_INTERRUPT_LEVEL' in mon,
'gptimer matches commit level':'config.intr_priority = kCommitInterruptLevel;' in mon,
'commit CPU0 compile guard':'CONFIG_ESP_TIMER_ISR_AFFINITY_CPU0' in mon,
'same-level ISR canary uses ISR dispatch':'isr_canary_args.dispatch_method = ESP_TIMER_ISR;' in mon,
'isr canary callback IRAM':'void IRAM_ATTR IsrCanaryCallback' in mon,
'isr canary 16.5s':'kIsrCanaryOffsetUs = 16500000LL' in mon,
'isr canary busywait':'esp_rom_delay_us(kIsrCanaryHoldUs);' in mon,
'task canary 15.5s':'kCanaryOffsetUs = 15500000LL' in mon,
'task canary critical section':'portENTER_CRITICAL(&s_canary_mux);' in mon and 'portEXIT_CRITICAL(&s_canary_mux);' in mon,
'no boundary early return':'if (ExpectedSampleIsGuarded(expected_local_us))' not in mon,
'near-boundary still retained':'const bool near_commit = ExpectedSampleIsGuarded(expected_local_us);' in mon and 'if (lateness_us >= kThresholdUs)' in mon,
'near-boundary only suppresses probe':'if (!near_commit)' in mon and 's_probe_suppressed_events' in mon,
'interrupted task sampler':'xTaskGetCurrentTaskHandleForCore(0)' in mon,
'interrupted task commit':'cpu0_latency_monitor_note_commit' in mon and 'event.interrupted_task = interrupted;' in mon,
'interrupted task ISR canary':'s_isr_canary.interrupted_task = xTaskGetCurrentTaskHandleForCore(0);' in mon,
'hardware auto reload':'alarm.flags.auto_reload_on_alarm = true;' in mon,
'no 4kHz rearm':'gptimer_set_alarm_action(timer, &next)' not in mon,
'monitor prepare':'cpu0_latency_monitor_prepare_run(command.command_id);' in disp,
'monitor begin':'cpu0_latency_monitor_begin_run(command.command_id, start_disciplined_us);' in disp,
'monitor commit target':'cpu0_latency_monitor_set_commit_target(target_local_us);' in disp,
'monitor commit note':'cpu0_latency_monitor_note_commit(' in disp,
'monitor dump':'cpu0_latency_monitor_dump_run();' in disp,
'isr canary telemetry':'CPU0_ISR_CANARY requested=' in mon,
'level telemetry':'sampler_intr_level=%d commit_intr_level=%d' in mon,
'runtime core telemetry':'wrong_core_callbacks=%u' in mon and 'xPortGetCoreID() != 0' in mon,
'workload telemetry':'heartbeat_traffic=%s' in mon,
'probe suppression telemetry':'probe_suppressed_events=%u' in mon,
'GCC15 header no IRAM duplicate':'IRAM_ATTR' not in (root/'components/factory_display/cpu0_latency_monitor.h').read_text(),
'start diag off both':'# CONFIG_FACTORY_START_EDGE_DIAGNOSTICS is not set' in e1 and '# CONFIG_FACTORY_START_EDGE_DIAGNOSTICS is not set' in e2,
'old mechanism off both':'# CONFIG_FACTORY_TIMING_MECHANISM_TEST is not set' in e1 and '# CONFIG_FACTORY_TIMING_MECHANISM_TEST is not set' in e2,
'flash diagnostic retained':'CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS=y' in e1 and 'CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS=y' in e2,
}
failed=[]
for k,v in checks.items():
    print(('PASS' if v else 'FAIL')+': '+k)
    if not v: failed.append(k)
print(f'SUMMARY={sum(checks.values())}/{len(checks)}')
if failed:
    print('FAILED='+', '.join(failed)); sys.exit(1)
