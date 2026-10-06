from pathlib import Path

root = Path(__file__).resolve().parents[1]
mon = (root / 'components/factory_display/cpu0_latency_monitor.cpp').read_text()
hdr = (root / 'components/factory_display/cpu0_latency_monitor.h').read_text()
disp_hdr = (root / 'components/factory_display/include/factory_display.h').read_text()
main = (root / 'main/main.cpp').read_text()
proto_h = (root / 'main/protocol_codec.h').read_text()
proto_cpp = (root / 'main/protocol_codec.cpp').read_text()
kconfig = (root / 'main/Kconfig.projbuild').read_text()

checks = {
    '4kHz 250us sampler retained': 'CONFIG_FACTORY_CPU0_LATENCY_PERIOD_US' in mon and 'alarm.alarm_count = kPeriodUs' in mon,
    'same interrupt level as commit': 'config.intr_priority = kCommitInterruptLevel;' in mon,
    'runtime wrong-core counter': 's_wrong_core_callbacks = s_wrong_core_callbacks + 1U;' in mon,
    'task canary removed': 'CanaryTask' not in mon and 'lat_canary' not in mon,
    'ISR canary removed': 'IsrCanaryCallback' not in mon and 'lat_isr_canary' not in mon,
    'GPIO probe removed': 'ProbePulse' not in mon and 'gpio_config_t probe' not in mon,
    'monitor summary API': 'cpu0_latency_monitor_get_summary' in hdr and 'CPU0_FLEET_SUMMARY' in mon,
    'monitor samples in public health': 'cpu0_monitor_samples' in disp_hdr,
    'worst task in public health': 'cpu0_monitor_worst_task[16]' in disp_hdr,
    'commit overlap in public health': 'cpu0_commit_overlap' in disp_hdr,
    'STATUS compact hex counters': '|%u|%X|%X|%X|%.*s|%X|%X|%u|%X|%X|%u"' in proto_cpp,
    'STATUS formatter accepts compact monitor args': 'cpu0_interrupt_level_match = false' in proto_h,
    'main exports monitor summary': 'display_health.cpu0_monitor_event_count' in main and 'cpu0_interrupt_level_match' in main,
    'monitor defaults on for classic ESP32': 'default y if IDF_TARGET_ESP32' in kconfig,
    'old probe/canary Kconfig removed': 'FACTORY_CPU0_LATENCY_PROBE_GPIO' not in kconfig and 'FACTORY_CPU0_LATENCY_CANARY_HOLD_US' not in kconfig,
}

for profile in ('esp01','esp02','esp03','esp04','esp05'):
    text = (root / f'sdkconfig.{profile}').read_text()
    checks[f'{profile} monitor enabled'] = 'CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y' in text
    checks[f'{profile} heartbeat suppressed'] = 'CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT=y' in text
    checks[f'{profile} flash diagnostic off'] = '# CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS is not set' in text
    checks[f'{profile} flash counters off'] = '# CONFIG_SPI_FLASH_ENABLE_COUNTERS is not set' in text
    checks[f'{profile} analyzer offset zero'] = 'CONFIG_FACTORY_ANALYZER_PRESENTATION_OFFSET_US=0' in text

failed = []
for name, ok in checks.items():
    print(f'{"PASS" if ok else "FAIL"}: {name}')
    if not ok:
        failed.append(name)

if failed:
    raise SystemExit(f'{len(failed)} v6.23.11 fleet-monitor checks failed: {failed}')
print(f'PASS: {len(checks)}/{len(checks)} v6.23.11 fleet-monitor source-contract checks')
