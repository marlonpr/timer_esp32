#!/usr/bin/env python3
from pathlib import Path
import re
ROOT=Path(__file__).resolve().parents[1]
if 'CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y' in (ROOT/'sdkconfig.esp02').read_text():
    print('SKIP: v6.23.9 CPU0 latency monitor supersedes this older diagnostic mode')
    raise SystemExit(0)
cpp=(ROOT/'main/flash_guard_diag.cpp').read_text(); main=(ROOT/'main/main.cpp').read_text()
esp01=(ROOT/'sdkconfig.esp01').read_text(); esp02=(ROOT/'sdkconfig.esp02').read_text()
if 'CONFIG_FACTORY_TIMING_MECHANISM_TEST=y' in esp02:
    print('SKIP: v6.23.6 positive-control runtime was intentionally superseded by v6.23.8 deterministic mechanism control')
    raise SystemExit(0)
checks=[]
def check(c,m):
    if not c: raise AssertionError(m)
    checks.append(m)
check('CONFIG_FACTORY_FLASH_GUARD_INDUCED_NVS_TEST=y' in esp02,'ESP02 induced mode enabled')
check('# CONFIG_FACTORY_FLASH_GUARD_INDUCED_NVS_TEST is not set' in esp01,'ESP01 remains control')
check('nvs_set_u32(s_nvs_handle, "pulse", event.value)' in cpp,'dummy NVS value changes')
check('nvs_commit(s_nvs_handle)' in cpp,'NVS commit forced')
check('flash_guard_diag_countdown_started(' in main,'positive control starts after RUNNING')
check('flash_guard_diag_countdown_finished();' in main,'positive control stops at FINISHED')
check('esp_timer_start_once' in cpp,'targeted scheduler used')
m=re.search(r'void InducedNvsTimerCallback\(void \*\) \{(.*?)\n\}',cpp,re.S)
check(m is not None,'timer callback found')
body=m.group(1)
check(all(x not in body for x in ('nvs_set_','nvs_commit(','nvs_open(','ESP_LOG')),'timer callback NVS/log free')
print(f'PASS: {len(checks)}/{len(checks)} v6.23.6+ induced-NVS invariants')
