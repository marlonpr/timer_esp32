#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
if 'CONFIG_FACTORY_CPU0_LATENCY_MONITOR=y' in (ROOT/'sdkconfig.esp02').read_text():
    print('SKIP: v6.23.9 CPU0 latency monitor supersedes this older diagnostic mode')
    raise SystemExit(0)
cpp = (ROOT / 'main/flash_guard_diag.cpp').read_text()
kconfig = (ROOT / 'main/Kconfig.projbuild').read_text()
esp01 = (ROOT / 'sdkconfig.esp01').read_text()
esp02 = (ROOT / 'sdkconfig.esp02').read_text()
checks = []

def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    checks.append(msg)

check('esp_private/cache_utils.h' in cpp, 'private cache control API included')
check('spi_flash_disable_interrupts_caches_and_other_cpu();' in cpp, 'direct cache-off injection present')
check('spi_flash_enable_interrupts_caches_and_other_cpu();' in cpp, 'direct cache restore present')
check('MechanismKind::CacheOff, 30, 0, 500, 1000' in cpp, 'boundary 30 is 1 ms cache-off centered on edge')
check('MechanismKind::Cpu0Critical, 60, 0, 100, 300' in cpp, 'boundary 60 CPU0 300 us critical starts 100 us early')
check('MechanismKind::Cpu1Critical, 90, 1, 100, 300' in cpp, 'boundary 90 CPU1 control mirrors CPU0 critical timing')
check('xTaskCreatePinnedToCore' in cpp, 'mechanism workers are core-pinned')
check('portENTER_CRITICAL(mux)' in cpp and 'portEXIT_CRITICAL(mux)' in cpp, 'private critical-section injection used')
check('ProbeHigh();' in cpp and 'ProbeLow();' in cpp, 'analyzer-visible probe wraps protected intervals')
check('GPIO_OUT_W1TS_REG' in cpp and 'GPIO_OUT_W1TC_REG' in cpp, 'probe uses IRAM/cache-safe direct GPIO registers')
check('CONFIG_FACTORY_TIMING_PROBE_GPIO=32' in esp02, 'ESP02 probe GPIO32 configured')
check('CONFIG_FACTORY_TIMING_MECHANISM_TEST=y' in esp02, 'ESP02 deterministic mechanism test enabled')
check('# CONFIG_FACTORY_START_EDGE_DIAGNOSTICS is not set' in esp02,
      'ESP02 START-edge diagnostic disabled so GPIO32 is dedicated to timing probe')
check('GPIO_OUT1_W1TS_REG' in cpp and 'GPIO_OUT1_W1TC_REG' in cpp,
      'timing probe supports GPIO32/33 through OUT1 W1TS/W1TC registers')

check('# CONFIG_FACTORY_TIMING_PROBE_OUTPUT is not set' in esp01, 'ESP01 remains probe/injection-free control')
check('# CONFIG_FACTORY_TIMING_MECHANISM_TEST is not set' in esp01, 'ESP01 deterministic injections disabled')
check('FACTORY_FLASH_GUARD_INDUCED_NVS_TEST' not in esp02 and 'FACTORY_FLASH_GUARD_NVS_TEST_LEAD_US' not in esp02, 'old NVS phase/target test removed to avoid confound')
check('CONFIG_FACTORY_FLASH_GUARD_EVENT_THRESHOLD_US=0' in esp02, 'ESP02 retains every flash OS window')
check('CONFIG_FACTORY_FLASH_GUARD_EVENT_THRESHOLD_US=0' in esp01, 'ESP01 also retains every observed flash OS window')
check('start_hook_enter_us=' in cpp and 'start_wait_us=' in cpp, 'flash start/IPC wait is separately reported')
check('cache_off_begin_us=' in cpp and 'cache_off_duration_us=' in cpp, 'cache-off interval is separately reported')
check('end_restore_us=' in cpp, 'flash end/restore time is separately reported')
check('TIMING_MECHANISM_EVENT' in cpp and 'protected_begin_minus_target_us=' in cpp, 'injected interval timing is logged relative to edge')
check('range 0 1000000' in kconfig and 'v6.23.8 uses 0 us' in kconfig, 'Kconfig permits zero-us flash retention threshold')

# Cache-off protected section must contain no logging/NVS and only IRAM-safe primitives.
m = re.search(r'void IRAM_ATTR InjectCacheOffWindow\([^)]*\)\s*\{(.*?)\n\}', cpp, re.S)
check(m is not None, 'cache-off injection function found')
body = m.group(1)
check(all(x not in body for x in ('ESP_LOG', 'nvs_', 'printf', 'malloc', 'new ')), 'cache-off section has no logging/NVS/allocation')

# Flash callbacks must not log or touch NVS while cache may be disabled.
for name in ('DiagnosticFlashOsStart', 'DiagnosticFlashOsEnd'):
    m = re.search(rf'esp_err_t IRAM_ATTR {name}\([^)]*\)\s*\{{(.*?)\n\}}', cpp, re.S)
    check(m is not None, f'{name} found')
    body = m.group(1)
    check(all(x not in body for x in ('ESP_LOG', 'nvs_set_', 'nvs_commit(', 'nvs_open(')), f'{name} remains cache-safe')

check('++s_total_operations' not in cpp and 's_total_duration_us +=' not in cpp, 'GCC15 volatile RMW regression absent')
print(f'PASS: {len(checks)}/{len(checks)} v6.23.8 deterministic mechanism-control checks')
