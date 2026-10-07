#!/usr/bin/env python3
"""Generate qualification-only 500 us START-task delay profiles.

The ordinary sdkconfig.espNN files are left unchanged. Use a separate build
directory for each diagnostic profile. Interrupts remain enabled during the
START delay; this tests the frozen epoch without delaying the COMMIT ISR.
"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
OPTION = 'CONFIG_FACTORY_START_TASK_TEST_DELAY_US'

def profile(base: str, delay_us: int = 500) -> str:
    lines = [line for line in base.splitlines()
             if not line.startswith(OPTION + '=')]
    return '\n'.join(lines) + f'\n# Qualification-only START-task delay; use a separate build directory.\n{OPTION}={delay_us}\n'

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--devices', nargs='+', default=[f'ESP{n:02d}' for n in range(1,6)])
    args = ap.parse_args()
    for device in args.devices:
        if not re.fullmatch(r'ESP(?:0[1-9]|1[0-5])', device):
            ap.error(f'invalid device: {device}')
        base = ROOT / f'sdkconfig.{device.lower()}'
        output = ROOT / f'sdkconfig.{device.lower()}.start_delay_500us'
        output.write_text(profile(base.read_text()))
        print(f'{output.name}: GENERATED delay_us=500')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
