#!/usr/bin/env python3
"""Generate/verify a one-board telemetry canary profile from an existing sdkconfig.

The output differs only in the CPU0 task-canary Kconfig lines. The monitor remains
at the normal fleet settings; the ISR canary stays disabled. This is intended for
one short end-to-end STATUS -> controller CSV validation run, never fleet use.
"""
from __future__ import annotations
import argparse, re
from pathlib import Path

CANARY_KEYS = {
    'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY': 'y',
    'CONFIG_FACTORY_CPU0_LATENCY_TASK_CANARY_MIDSECOND': None,
    'CONFIG_FACTORY_CPU0_LATENCY_ISR_CANARY': None,
    'CONFIG_FACTORY_CPU0_LATENCY_CANARY_BOUNDARY': '10',
    'CONFIG_FACTORY_CPU0_LATENCY_CANARY_LEAD_US': '150',
    'CONFIG_FACTORY_CPU0_LATENCY_CANARY_HOLD_US': '600',
}

def set_kconfig(text: str, key: str, value: str | None) -> str:
    set_re = re.compile(rf'^{re.escape(key)}=.*$', re.M)
    unset_re = re.compile(rf'^# {re.escape(key)} is not set$', re.M)
    if value is None:
        replacement = f'# {key} is not set'
    else:
        replacement = f'{key}={value}'
    if set_re.search(text):
        return set_re.sub(replacement, text, count=1)
    if unset_re.search(text):
        return unset_re.sub(replacement, text, count=1)
    return text.rstrip() + '\n' + replacement + '\n'

def strip_canary_lines(text: str) -> str:
    lines=[]
    for line in text.splitlines():
        if any(line.startswith(k+'=') or line == f'# {k} is not set' for k in CANARY_KEYS):
            continue
        lines.append(line)
    return '\n'.join(lines) + '\n'

def main() -> int:
    ap=argparse.ArgumentParser()
    ap.add_argument('base', help='base profile, e.g. sdkconfig.esp01 or sdkconfig.esp03')
    ap.add_argument('--output', help='default: <base>.telemetry_canary')
    ap.add_argument('--check-only', action='store_true')
    args=ap.parse_args()
    root=Path(__file__).resolve().parents[1]
    base_path=root/args.base
    out_path=root/(args.output or (args.base+'.telemetry_canary'))
    base=base_path.read_text()
    expected=base
    for key,value in CANARY_KEYS.items(): expected=set_kconfig(expected,key,value)
    if not args.check_only: out_path.write_text(expected)
    if not out_path.exists():
        print(f'{out_path.name}: MISSING')
        return 1
    actual=out_path.read_text()
    semantic_ok = strip_canary_lines(actual)==strip_canary_lines(base)
    for key,value in CANARY_KEYS.items():
        wanted = f'# {key} is not set' if value is None else f'{key}={value}'
        semantic_ok = semantic_ok and wanted in actual.splitlines()
    ok=semantic_ok
    device=re.search(r'^CONFIG_FACTORY_DEVICE_ID="([^"]+)"$',base,re.M)
    dev=device.group(1) if device else 'UNKNOWN'
    print(f'{out_path.name}: {"PASS" if ok else "FAIL"} telemetry-canary-only profile device={dev}')
    return 0 if ok else 1

if __name__=='__main__': raise SystemExit(main())
