from pathlib import Path
root=Path(__file__).resolve().parents[1]
s=(root/'components/factory_display/cpu0_latency_monitor.cpp').read_text()
cfg=(root/'sdkconfig.esp01.telemetry_canary').read_text()
checks={
 'classic fleet tick rate 100Hz':'CONFIG_FREERTOS_HZ=100' in cfg,
 'no zero tick 1ms canary poll':'vTaskDelay(pdMS_TO_TICKS(1));' not in s,
 'canary poll blocks one full tick':'vTaskDelay(1);' in s,
 'task canary still build optional':'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY' in s,
 'canary hold remains 600us':'CONFIG_FACTORY_CPU0_LATENCY_CANARY_HOLD_US=600' in cfg,
 'canary boundary remains 10':'CONFIG_FACTORY_CPU0_LATENCY_CANARY_BOUNDARY=10' in cfg,
 'canary lead remains 150us':'CONFIG_FACTORY_CPU0_LATENCY_CANARY_LEAD_US=150' in cfg,
}
for k,v in checks.items(): print(f'{k}: {"PASS" if v else "FAIL"}')
raise SystemExit(0 if all(checks.values()) else 1)
