from pathlib import Path
import re, sys
root=Path(__file__).resolve().parents[1]
h=(root/'components/factory_display/cpu0_latency_monitor.h').read_text()
cpp=(root/'components/factory_display/cpu0_latency_monitor.cpp').read_text()
checks = {
    'public declaration has no repeated IRAM_ATTR': 'IRAM_ATTR cpu0_latency_monitor_note_commit' not in h,
    'definition remains IRAM_ATTR': 'void IRAM_ATTR cpu0_latency_monitor_note_commit' in cpp,
    'header no longer needs esp_attr': '#include "esp_attr.h"' not in h,
}
failed=[k for k,v in checks.items() if not v]
for k,v in checks.items(): print(('PASS' if v else 'FAIL')+': '+k)
if failed: sys.exit(1)
print(f'PASS: {len(checks)}/{len(checks)} GCC15 IRAM declaration checks')
