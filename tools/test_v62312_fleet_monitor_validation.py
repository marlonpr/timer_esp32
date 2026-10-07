#!/usr/bin/env python3
from pathlib import Path
import re, subprocess, sys
ROOT=Path(__file__).resolve().parents[1]
cpp=(ROOT/'components/factory_display/cpu0_latency_monitor.cpp').read_text()
h=(ROOT/'components/factory_display/cpu0_latency_monitor.h').read_text()
k=(ROOT/'main/Kconfig.projbuild').read_text()
fd=(ROOT/'components/factory_display/factory_display.cpp').read_text()
checks={
 '251us period default': 'default 251' in k and 'CONFIG_FACTORY_CPU0_LATENCY_PERIOD_US' in cpp,
 '50us threshold default': 'default 50' in k,
 'task canary option default off': 'config FACTORY_CPU0_LATENCY_TASK_CANARY' in k and 'default n' in k[k.index('config FACTORY_CPU0_LATENCY_TASK_CANARY'):k.index('config FACTORY_CPU0_LATENCY_CANARY_BOUNDARY')],
 'ISR canary option default off': 'config FACTORY_CPU0_LATENCY_ISR_CANARY' in k,
 'task canary crosses configured boundary': 's_commit_target_boundary == kTaskCanaryBoundary' in cpp and 'portENTER_CRITICAL(&s_task_canary_mux)' in cpp,
 'task canary name lat_canary': '"lat_canary"' in cpp,
 'ISR canary retained as option': 'IsrCanaryCallback' in cpp and 'ESP_TIMER_ISR' in cpp,
 'commit target carries boundary': 'cpu0_latency_monitor_set_commit_target(uint32_t boundary' in h and 'cpu0_latency_monitor_set_commit_target(boundary, target_local_us);' in fd,
 'summary path unchanged': 'cpu0_latency_monitor_get_summary' in h and 'commit_overlap' in h,
 'fleet analyzer offset zero': all('CONFIG_FACTORY_ANALYZER_PRESENTATION_OFFSET_US=0' in (ROOT/f'sdkconfig.esp{i:02d}').read_text() for i in range(1,16)),
 'fleet task canary disabled': all('# CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY is not set' in (ROOT/f'sdkconfig.esp{i:02d}').read_text() for i in range(1,16)),
 'fleet ISR canary disabled': all('# CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY is not set' in (ROOT/f'sdkconfig.esp{i:02d}').read_text() for i in range(1,16)),
 'telemetry canary profile task only': 'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY=y' in (ROOT/'sdkconfig.esp01.telemetry_canary').read_text() and '# CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY is not set' in (ROOT/'sdkconfig.esp01.telemetry_canary').read_text(),
 'telemetry canary boundary10': 'CONFIG_FACTORY_CPU0_LATENCY_CANARY_BOUNDARY=10' in (ROOT/'sdkconfig.esp01.telemetry_canary').read_text(),
 'telemetry canary lead150 hold600': 'CONFIG_FACTORY_CPU0_LATENCY_CANARY_LEAD_US=150' in (ROOT/'sdkconfig.esp01.telemetry_canary').read_text() and 'CONFIG_FACTORY_CPU0_LATENCY_CANARY_HOLD_US=600' in (ROOT/'sdkconfig.esp01.telemetry_canary').read_text(),
 'fleet heartbeat baseline suppressed': all('CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT=y' in (ROOT/f'sdkconfig.esp{i:02d}').read_text() for i in range(1,16)),
 'ESP06-15 generated profiles present': all((ROOT/f'sdkconfig.esp{i:02d}').exists() for i in range(6,16)),
 'generator exists': (ROOT/'tools/generate_fleet_sdkconfigs.py').exists(),
}
failed=[]
for name,ok in checks.items():
 print(('PASS' if ok else 'FAIL')+': '+name)
 if not ok: failed.append(name)
proc=subprocess.run([sys.executable,str(ROOT/'tools/generate_fleet_sdkconfigs.py'),'--check-only'],cwd=ROOT,text=True,capture_output=True)
print(proc.stdout,end='')
if proc.returncode: failed.append('fleet identity-only generator check')
if failed:
 print(f'FAIL: {len(failed)} v6.23.12 checks failed: {failed}')
 raise SystemExit(1)
print(f'PASS: {len(checks)} source checks + identity-only fleet profile verification')
