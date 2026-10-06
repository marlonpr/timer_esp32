#!/usr/bin/env python3
from pathlib import Path
import subprocess, sys
ROOT=Path(__file__).resolve().parents[1]
main=(ROOT/'main/main.cpp').read_text()
k=(ROOT/'main/Kconfig.projbuild').read_text()
checks={
 'fleet monitor remains enabled ESP01-15': all('CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y' in (ROOT/f'sdkconfig.esp{i:02d}').read_text() for i in range(1,16)),
 'fleet task canary remains off ESP01-15': all('# CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY is not set' in (ROOT/f'sdkconfig.esp{i:02d}').read_text() for i in range(1,16)),
 'fleet ISR canary remains off ESP01-15': all('# CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY is not set' in (ROOT/f'sdkconfig.esp{i:02d}').read_text() for i in range(1,16)),
 'task canary Kconfig default off': 'config FACTORY_CPU0_LATENCY_TASK_CANARY' in k and 'default n' in k[k.index('config FACTORY_CPU0_LATENCY_TASK_CANARY'):k.index('config FACTORY_CPU0_LATENCY_CANARY_BOUNDARY')],
 'ESP01 canary profile exists': (ROOT/'sdkconfig.esp01.telemetry_canary').exists(),
 'all fleet targets are classic ESP32': all('CONFIG_IDF_TARGET_ESP32=y' in (ROOT/f'sdkconfig.esp{i:02d}').read_text() for i in range(1,16)),
 'unique DHCP hostname derived from device ID': 'esp_netif_set_hostname(wifi_sta_netif, CONFIG_FACTORY_DEVICE_ID)' in main,
 'default DHCP station path retained': 'esp_netif_create_default_wifi_sta()' in main,
 'no static IP assignment path': main.count('esp_netif_set_ip_info(')==1 and 'const esp_netif_ip_info_t empty_ip_info{};' in main,
 'fleet analyzer offset remains zero': all('CONFIG_FACTORY_ANALYZER_PRESENTATION_OFFSET_US=0' in (ROOT/f'sdkconfig.esp{i:02d}').read_text() for i in range(1,16)),
 'heartbeat-on generator exists': (ROOT/'tools/generate_heartbeat_on_profiles.py').exists(),
 'heartbeat-on variants ESP01-15 present': all((ROOT/f'sdkconfig.esp{i:02d}.heartbeat_on').exists() for i in range(1,16)),
}
failed=[]
for name,ok in checks.items():
 print(('PASS' if ok else 'FAIL')+': '+name)
 if not ok: failed.append(name)
for base in ('sdkconfig.esp01',):
 proc=subprocess.run([sys.executable,str(ROOT/'tools/generate_telemetry_canary_profile.py'),base,'--check-only'],cwd=ROOT,text=True,capture_output=True)
 print(proc.stdout,end='')
 if proc.returncode: failed.append(base+' telemetry canary invariant')
proc=subprocess.run([sys.executable,str(ROOT/'tools/generate_fleet_sdkconfigs.py'),'--check-only'],cwd=ROOT,text=True,capture_output=True)
print(proc.stdout,end='')
if proc.returncode: failed.append('fleet profile/network identity verification')
proc=subprocess.run([sys.executable,str(ROOT/'tools/generate_heartbeat_on_profiles.py'),'--start','1','--end','15','--check-only'],cwd=ROOT,text=True,capture_output=True)
print(proc.stdout,end='')
if proc.returncode: failed.append('heartbeat-on profile verification')
if failed:
 print(f'FAIL: {len(failed)} v6.23.13 checks failed: {failed}')
 raise SystemExit(1)
print(f'PASS: {len(checks)} rollout-hardening source checks + ESP01 canary invariant + fleet DHCP/identity verification')
