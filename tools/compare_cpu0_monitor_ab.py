#!/usr/bin/env python3
"""Compare retained COMMIT timing from monitor-ON and monitor-OFF serial logs.

This is a lightweight rollout A/B check. The ISR_PUBLISH trace retains first/worst
records rather than every boundary, so report it as retained-trace evidence, not a
full-distribution estimator.
"""
from __future__ import annotations
import argparse,re,statistics
from pathlib import Path
PAT=re.compile(r'ISR_PUBLISH boundary=(\d+).*?callback_lateness_us=(\d+).*?publish_marker_us=(\d+)')
FIN=re.compile(r'Visual countdown finished:.*?worst_isr_publish_marker_lateness_us=(\d+)')

def parse(path:Path):
    text=path.read_text(errors='replace')
    vals=[(int(a),int(b),int(c)) for a,b,c in PAT.findall(text)]
    fin=[int(x) for x in FIN.findall(text)]
    return vals, fin[-1] if fin else None

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('monitor_on'); ap.add_argument('monitor_off'); a=ap.parse_args()
    for label,name in [('MONITOR_ON',a.monitor_on),('MONITOR_OFF',a.monitor_off)]:
        vals,worst=parse(Path(name)); l=[x[1] for x in vals]
        print(f'{label}_RETAINED_COUNT={len(l)}')
        if l:
            print(f'{label}_RETAINED_MIN_US={min(l)}')
            print(f'{label}_RETAINED_MEDIAN_US={statistics.median(l):.1f}')
            print(f'{label}_RETAINED_MAX_US={max(l)}')
        print(f'{label}_RUN_WORST_MARKER_LATENESS_US={worst if worst is not None else "N/A"}')
    return 0
if __name__=='__main__': raise SystemExit(main())
