#!/usr/bin/env python3
from pathlib import Path
import re
ROOT=Path(__file__).resolve().parents[1]
if 'CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y' in (ROOT/'sdkconfig.esp02').read_text():
    print('SKIP: v6.23.9 CPU0 latency monitor supersedes this older diagnostic mode')
    raise SystemExit(0)
cpp=(ROOT/'main/flash_guard_diag.cpp').read_text()
kconfig=(ROOT/'main/Kconfig.projbuild').read_text()
esp01=(ROOT/'sdkconfig.esp01').read_text()
esp02=(ROOT/'sdkconfig.esp02').read_text()
if 'CONFIG_FACTORY_TIMING_MECHANISM_TEST=y' in esp02:
    print('SKIP: v6.23.7 positive-control runtime was intentionally superseded by v6.23.8 deterministic mechanism control')
    raise SystemExit(0)
checks=[]
def check(c,m):
    if not c: raise AssertionError(m)
    checks.append(m)
check('esp_flash_default_chip->os_func' in cpp, 'main esp_flash OS path wrapped')
check('s_diagnostic_flash_os = *current;' in cpp, 'all original OS callbacks copied')
check('DiagnosticFlashOsStart' in cpp and 'original->start(arg, flags)' in cpp, 'start delegates original')
check('DiagnosticFlashOsEnd' in cpp and 'original->end(arg)' in cpp, 'end delegates original')
check('IRAM_ATTR DiagnosticFlashOsStart' in cpp and 'IRAM_ATTR DiagnosticFlashOsEnd' in cpp, 'OS wrappers IRAM')
check('spi_flash_guard_set' not in cpp and 'spi_flash_guard_get' not in cpp, 'legacy global guard hook removed')
check('CONFIG_SPI_FLASH_ENABLE_COUNTERS=y' in esp01 and 'CONFIG_SPI_FLASH_ENABLE_COUNTERS=y' in esp02, 'official flash counters enabled')
check('esp_flash_get_counters()' in cpp, 'official counter snapshot used')
check('kNvsTargetBoundaries[] = {30, 60, 90}' in cpp, 'three disciplined target boundaries')
check('rtc_discipline_disciplined_to_local_us' in cpp, 'targets use disciplined inverse mapping')
check('CONFIG_FACTORY_FLASH_GUARD_NVS_TEST_LEAD_US=500' in esp02, 'ESP02 0.5 ms commit lead configured')
check('CONFIG_FACTORY_FLASH_GUARD_INDUCED_NVS_TEST=y' in esp02, 'ESP02 induced NVS enabled')
check('# CONFIG_FACTORY_FLASH_GUARD_INDUCED_NVS_TEST is not set' in esp01, 'ESP01 remains unstressed control')
check('nvs_set_u32(s_nvs_handle, "pulse", event.value)' in cpp and 'nvs_commit(s_nvs_handle)' in cpp, 'real NVS set+commit retained')
check('FLASH_OS_EVENT' in cpp and 'flash_operations=' in cpp, 'per-write flash OS correlation logged')
check('write_count_delta=' in cpp and 'write_time_us_delta=' in cpp, 'independent official flash counters logged')
check('esp_timer_start_once' in cpp and 'esp_timer_start_periodic' not in cpp, 'targeted one-shot scheduling replaces phase sweep')
# Timer callback may only notify.
m=re.search(r'void InducedNvsTimerCallback\(void \*\) \{(.*?)\n\}',cpp,re.S)
check(m is not None, 'timer callback found')
body=m.group(1)
check(all(x not in body for x in ('nvs_set_','nvs_commit(','nvs_open(','ESP_LOG')), 'timer callback NVS/log free')
# Cache-disabled wrappers may not call logging or NVS.
for name in ('DiagnosticFlashOsStart','DiagnosticFlashOsEnd'):
    m=re.search(rf'esp_err_t IRAM_ATTR {name}\([^)]*\) \{{(.*?)\n\}}',cpp,re.S)
    check(m is not None,f'{name} found')
    body=m.group(1)
    check(all(x not in body for x in ('ESP_LOG','nvs_set_','nvs_commit(','nvs_open(')),f'{name} logging/NVS free')
check('++s_total_operations' not in cpp and 's_total_duration_us +=' not in cpp, 'GCC15 volatile RMW regression absent')
print(f'PASS: {len(checks)}/{len(checks)} v6.23.7 flash-OS targeted positive-control checks')
