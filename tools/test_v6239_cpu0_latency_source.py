from pathlib import Path
import re, sys

root = Path(__file__).resolve().parents[1]
mon = (root/'components/factory_display/cpu0_latency_monitor.cpp').read_text()
disp = (root/'components/factory_display/factory_display.cpp').read_text()
cmake = (root/'components/factory_display/CMakeLists.txt').read_text()
kcfg = (root/'main/Kconfig.projbuild').read_text()
esp01 = (root/'sdkconfig.esp01').read_text()
esp02 = (root/'sdkconfig.esp02').read_text()

checks = {
    'monitor source in component': 'cpu0_latency_monitor.cpp' in cmake,
    'gptimer dependency': 'esp_driver_gptimer' in cmake,
    'monitor enabled esp01': 'CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y' in esp01,
    'monitor enabled esp02': 'CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y' in esp02,
    '250us esp01': 'CONFIG_FACTORY_CPU0_LATENCY_PERIOD_US=250' in esp01,
    '250us esp02': 'CONFIG_FACTORY_CPU0_LATENCY_PERIOD_US=250' in esp02,
    '50us threshold': 'CONFIG_FACTORY_CPU0_LATENCY_THRESHOLD_US=50' in esp01 and 'CONFIG_FACTORY_CPU0_LATENCY_THRESHOLD_US=50' in esp02,
    '400us guard': 'CONFIG_FACTORY_CPU0_LATENCY_BOUNDARY_GUARD_US=400' in esp01 and 'CONFIG_FACTORY_CPU0_LATENCY_BOUNDARY_GUARD_US=400' in esp02,
    'GPIO32 probes': 'CONFIG_FACTORY_CPU0_LATENCY_PROBE_GPIO=32' in esp01 and 'CONFIG_FACTORY_CPU0_LATENCY_PROBE_GPIO=32' in esp02,
    'START diag off esp01': '# CONFIG_FACTORY_START_EDGE_DIAGNOSTICS is not set' in esp01,
    'START diag off esp02': '# CONFIG_FACTORY_START_EDGE_DIAGNOSTICS is not set' in esp02,
    'traffic enabled esp01': '# CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT is not set' in esp01,
    'traffic enabled esp02': '# CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT is not set' in esp02,
    'old mechanism off esp01': '# CONFIG_FACTORY_TIMING_MECHANISM_TEST is not set' in esp01,
    'old mechanism off esp02': '# CONFIG_FACTORY_TIMING_MECHANISM_TEST is not set' in esp02,
    'old timing probe off esp01': '# CONFIG_FACTORY_TIMING_PROBE_OUTPUT is not set' in esp01,
    'old timing probe off esp02': '# CONFIG_FACTORY_TIMING_PROBE_OUTPUT is not set' in esp02,
    'gptimer control IRAM': 'CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM=y' in esp01 and 'CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM=y' in esp02,
    'level1 gptimer': 'config.intr_priority = 1;' in mon,
    '1MHz gptimer': 'config.resolution_hz = 1000000;' in mon,
    'hardware periodic auto reload': 'alarm.flags.auto_reload_on_alarm = true;' in mon,
    'no 4kHz driver rearm': 'gptimer_set_alarm_action(timer, &next)' not in mon and 'next.flags.auto_reload_on_alarm' not in mon,
    'software absolute schedule': 's_expected_sample_local_us' in mon and 'periods_elapsed' in mon,
    'boundary guard minimal path': 's_guarded_samples' in mon and 'ExpectedSampleIsGuarded(expected_local_us)' in mon,
    'interrupted task captured': 'xTaskGetCurrentTaskHandleForCore(0)' in mon,
    'task inventory': 'uxTaskGetSystemState' in mon and 'FindTaskIdentity' in mon,
    'canary 15.5s': 'kCanaryOffsetUs = 15500000LL' in mon,
    'canary critical': 'portENTER_CRITICAL(&s_canary_mux)' in mon and 'esp_rom_delay_us(kCanaryHoldUs)' in mon,
    'canary task named': '"lat_canary"' in mon,
    'commit target guard': 'cpu0_latency_monitor_set_commit_target(target_local_us);' in disp,
    'commit task attribution': 'cpu0_latency_monitor_note_commit(' in disp,
    'monitor prep before START': 'cpu0_latency_monitor_prepare_run(command.command_id);' in disp,
    'monitor begin at disciplined start': 'cpu0_latency_monitor_begin_run(command.command_id, start_disciplined_us);' in disp,
    'monitor end': 'cpu0_latency_monitor_end_run();' in disp,
    'monitor dump': 'cpu0_latency_monitor_dump_run();' in disp,
    'probe lower/upper regs': 'GPIO_OUT_W1TS_REG' in mon and 'GPIO_OUT1_W1TS_REG' in mon,
    'ESP01 presentation offset 0': 'CONFIG_FACTORY_ANALYZER_PRESENTATION_OFFSET_US=0' in esp01,
    'ESP02 presentation offset 50': 'CONFIG_FACTORY_ANALYZER_PRESENTATION_OFFSET_US=50' in esp02,
    'offset applied before arm': 'target_local_us = PresentationTargetLocalUs(target_local_us);' in disp,
    'offset in trace': 'PresentationTargetLocalUs(boundary_local_us)' in disp,
    'Kconfig monitor': 'config FACTORY_CPU0_LATENCY_MONITOR' in kcfg,
    'Kconfig presentation offset': 'config FACTORY_ANALYZER_PRESENTATION_OFFSET_US' in kcfg,
    'flash diagnostic retained': 'CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS=y' in esp01 and 'CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS=y' in esp02,
    'no mechanism injection warning source active': 'CONFIG_FACTORY_TIMING_MECHANISM_TEST=y' not in esp01 and 'CONFIG_FACTORY_TIMING_MECHANISM_TEST=y' not in esp02,
}

failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(('PASS' if ok else 'FAIL') + ': ' + name)
print(f"SUMMARY={sum(checks.values())}/{len(checks)}")
if failed:
    print('FAILED=' + ', '.join(failed))
    sys.exit(1)
