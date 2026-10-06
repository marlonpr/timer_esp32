#!/usr/bin/env python3
from __future__ import annotations
from pathlib import Path
import re, subprocess, sys

ROOT=Path(__file__).resolve().parents[1]
src=(ROOT/'components/factory_display/cpu0_latency_monitor.cpp').read_text()
main=(ROOT/'main/main.cpp').read_text()
kcfg=(ROOT/'main/Kconfig.projbuild').read_text()
codec=(ROOT/'main/protocol_codec.cpp').read_text()
profile=(ROOT/'sdkconfig.esp01').read_text()
task=(ROOT/'sdkconfig.esp01.sampler_task_canary').read_text()
isr=(ROOT/'sdkconfig.esp01.isr_canary').read_text()
monoff=(ROOT/'sdkconfig.esp01.monitor_off').read_text()

checks={
 'hardware lateness count-alarm':'count_value - alarm_value' in src,
 'free-running one-shot auto reload disabled':'auto_reload_on_alarm = false' in src,
 'dynamic next absolute alarm':'next_alarm_count = alarm_value +' in src and 'skipped_periods64 + 1ULL' in src,
 'missed periods from lateness':'late_ticks / static_cast<uint64_t>(kPeriodUs)' in src,
 'DRAM alarm config':'DRAM_ATTR gptimer_alarm_config_t s_next_alarm_config' in src,
 'ISR uses DRAM alarm config':'gptimer_set_alarm_action(timer, &s_next_alarm_config)' in src,
 'no ISR esp_timer timestamp':'actual_local_us = esp_timer_get_time()' not in src,
 'raw task handle only in ISR':'const TaskHandle_t interrupted = xTaskGetCurrentTaskHandleForCore(0);' in src,
 'task names resolved postrun':'FindTaskIdentity(s_worst_event_task)' in src,
 'GPTimer ISR cache safe required':'CONFIG_GPTIMER_ISR_CACHE_SAFE' in src,
 'GPTimer object cache safe required':'CONFIG_GPTIMER_OBJ_CACHE_SAFE' in src,
 'GPTimer handler IRAM required':'CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM' in src,
 'GPTimer controls IRAM required':'CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM' in src,
 'callback IRAM':'bool IRAM_ATTR OnAlarm' in src,
 'full RTC fit gate 129':'constexpr uint16_t kRtcStartMinimumFitPoints = 129;' in main,
 'RTC RMS gate 3us':'constexpr double kRtcStartMaximumFitRmsUs = 3.0;' in main,
 'ELF SHA status identity':'esp_app_get_elf_sha256' in main and 'FirmwareElfSha8()' in main,
 '44-field suffix in codec':'cpu0_monitor_missed_periods' in codec and 'firmware_elf_sha8' in codec,
 'fleet monitor enabled':'CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y' in profile,
 'fleet cache safe':'CONFIG_GPTIMER_ISR_CACHE_SAFE=y' in profile and 'CONFIG_GPTIMER_OBJ_CACHE_SAFE=y' in profile,
 'fleet task canary off':'# CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY is not set' in profile,
 'fleet ISR canary off':'# CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY is not set' in profile,
 '1000us midsecond task canary':'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND=y' in task and 'CONFIG_FACTORY_CPU0_LATENCY_CANARY_HOLD_US=1000' in task,
 '1000us ISR canary':'CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY_HOLD_US=1000' in isr,
 'monitor-off A/B profile':'# CONFIG_FACTORY_CPU0_LATENCY_MONITOR is not set' in monoff,
}

# Arithmetic regression: a 605 us blocker with first sample 34 us after mask
# must retain 571 us or more depending service overhead, not one period less.
period=250
hold=605
first_due_after_begin=34
service_after_end=6
late=(hold-first_due_after_begin)+service_after_end
missed=late//period
checks['605us arithmetic synthetic lateness 577']=late==577
checks['605us arithmetic synthetic missed2']=missed==2
# Any exact 1000 us block must leave the first pending deadline >=750 us late.
min_late=min(1000-d for d in range(1,period+1))
checks['1000us grid guarantee >=750']=min_late==750

for k,v in checks.items(): print(f'{k}: {"PASS" if v else "FAIL"}')

# Strong fleet invariant.
r=subprocess.run([sys.executable,str(ROOT/'tools/generate_fleet_sdkconfigs.py'),'--start','1','--end','15','--check-only'],text=True,capture_output=True)
print(r.stdout,end='')
checks['ESP01-ESP15 identity-only']=r.returncode==0 and 'FLEET_PROFILE_VERIFY=PASS range=ESP01-ESP15' in r.stdout

print(f'V6_23_15_SAMPLER_HARDENING={"PASS" if all(checks.values()) else "FAIL"}')
raise SystemExit(0 if all(checks.values()) else 1)
