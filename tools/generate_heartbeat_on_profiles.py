#!/usr/bin/env python3
"""Generate optional heartbeat-on variants from existing fleet sdkconfigs.

Only CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT may differ from each source
profile. Baseline fleet profiles remain heartbeat-suppressed to reproduce the
historical stall workload.
"""
from __future__ import annotations
import argparse
from pathlib import Path

KEY='CONFIG_FACTORY_SUPPRESS_RUNNING_HEARTBEAT'

def enable_heartbeat(text: str) -> str:
    enabled=f'{KEY}=y'
    disabled=f'# {KEY} is not set'
    if enabled in text:
        return text.replace(enabled, disabled, 1)
    if disabled in text:
        return text
    raise ValueError(f'{KEY} not found')

def normalize(text: str) -> str:
    return text.replace(f'{KEY}=y', f'# {KEY} is not set')

def main() -> int:
    ap=argparse.ArgumentParser()
    ap.add_argument('--start', type=int, default=1)
    ap.add_argument('--end', type=int, default=15)
    ap.add_argument('--check-only', action='store_true')
    args=ap.parse_args()
    root=Path(__file__).resolve().parents[1]
    failures=0
    for n in range(args.start,args.end+1):
        src=root/f'sdkconfig.esp{n:02d}'
        dst=root/f'sdkconfig.esp{n:02d}.heartbeat_on'
        if not src.exists():
            print(f'ESP{n:02d}: MISSING {src.name}'); failures+=1; continue
        base=src.read_text()
        try: expected=enable_heartbeat(base)
        except ValueError as e:
            print(f'ESP{n:02d}: FAIL {e}'); failures+=1; continue
        if not args.check_only: dst.write_text(expected)
        if not dst.exists():
            print(f'ESP{n:02d}: MISSING {dst.name}'); failures+=1; continue
        actual=dst.read_text()
        # Exact expected content ensures no settings other than heartbeat suppression drift.
        ok=(actual==expected)
        print(f'ESP{n:02d}: {"PASS" if ok else "FAIL"} heartbeat-on-only profile')
        if not ok: failures+=1
    print(f'HEARTBEAT_PROFILE_VERIFY={"PASS" if failures==0 else "FAIL"} range=ESP{args.start:02d}-ESP{args.end:02d}')
    return 0 if failures==0 else 1

if __name__=='__main__': raise SystemExit(main())
