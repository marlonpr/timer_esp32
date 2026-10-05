#!/usr/bin/env python3
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
diag=(ROOT/'main/flash_guard_diag.cpp').read_text()
main=(ROOT/'main/main.cpp').read_text()
config1=(ROOT/'sdkconfig.esp01').read_text(); config2=(ROOT/'sdkconfig.esp02').read_text()
checks={
 'main flash OS wrapper': 'esp_flash_default_chip->os_func' in diag,
 'start wrapper IRAM': 'IRAM_ATTR DiagnosticFlashOsStart' in diag,
 'end wrapper IRAM': 'IRAM_ATTR DiagnosticFlashOsEnd' in diag,
 'post-run dump call': 'flash_guard_diag_dump_run();' in main,
 'arm baseline call': 'flash_guard_diag_begin_run(command.command_id, local_start_us);' in main,
 'ESP01 diagnostic enabled': 'CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS=y' in config1,
 'ESP02 diagnostic enabled': 'CONFIG_FACTORY_FLASH_GUARD_DIAGNOSTICS=y' in config2,
 'legacy guard hook removed': 'spi_flash_guard_set' not in diag,
 'GCC15 volatile increment absent': '++s_total_operations' not in diag,
 'GCC15 volatile compound add absent': 's_total_duration_us +=' not in diag,
}
failed=[k for k,v in checks.items() if not v]
if failed: raise SystemExit('FAIL: '+', '.join(failed))
print(f'PASS: {len(checks)}/{len(checks)} v6.23.5+ diagnostic invariants')
