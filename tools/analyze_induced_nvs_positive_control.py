#!/usr/bin/env python3
"""Analyze v6.23.6 induced-NVS positive-control serial + Analyzer_v8 logs."""
from __future__ import annotations
import argparse, math, re, statistics
from pathlib import Path

ARM_RE = re.compile(r'FLASH_GUARD_RUN_ARM .*?target_local_us=(?P<target>-?\d+)')
DIAG_RE = re.compile(r'INDUCED_NVS_DIAG .*?requested=(?P<requested>\d+) completed=(?P<completed>\d+).*?timer_start_err=(?P<start>\S+) timer_stop_err=(?P<stop>\S+)')
EVENT_RE = re.compile(
    r'INDUCED_NVS_EVENT .*?sequence=(?P<seq>\d+) '
    r'scheduled_local_us=(?P<sched>-?\d+) begin_local_us=(?P<begin>-?\d+) '
    r'begin_error_us=(?P<beginerr>-?\d+) end_local_us=(?P<end>-?\d+) '
    r'duration_us=(?P<dur>-?\d+) value=(?P<value>\d+) guard_operations=(?P<ops>\d+) '
    r'set_err=(?P<set>\S+) commit_err=(?P<commit>\S+)')
FLASH_DIAG_RE = re.compile(r'FLASH_GUARD_DIAG .*?operations=(?P<ops>\d+) .*?total_us=(?P<total>\d+) .*?max_retained_us=(?P<max>\d+)')
EDGE_RE = re.compile(r'ANZ\|EDGE\|[^|]*\|[^|]*\|[^|]*\|(?P<name>ESP0[12]_COMMIT)\|(?P<t>\d+)\|')
COUNTS_RE = re.compile(r'ANZ\|COUNTS\|.*?dropped=(?P<dropped>\d+)')

def fit_line(y: list[int]):
    n=len(y)
    xbar=(n-1)/2.0
    ybar=sum(y)/n
    sxx=sum((i-xbar)**2 for i in range(n))
    slope=sum((i-xbar)*(v-ybar) for i,v in enumerate(y))/sxx
    intercept=ybar-slope*xbar
    res=[v-(intercept+slope*i) for i,v in enumerate(y)]
    rms=math.sqrt(sum(r*r for r in res)/n)
    return slope, intercept, res, rms

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('serial_log')
    ap.add_argument('analyzer_log')
    ap.add_argument('--device', default='ESP02')
    ap.add_argument('--near-boundary-us', type=int, default=20000)
    args=ap.parse_args()

    serial=Path(args.serial_log).read_text(errors='replace')
    analyzer=Path(args.analyzer_log).read_text(errors='replace')
    arm=ARM_RE.search(serial)
    diag=DIAG_RE.search(serial)
    fdiag=FLASH_DIAG_RE.search(serial)
    events={int(m['seq']):m.groupdict() for m in EVENT_RE.finditer(serial)}
    events=[events[k] for k in sorted(events)]

    commits={'ESP01':[], 'ESP02':[]}
    for m in EDGE_RE.finditer(analyzer):
        dev=m['name'].split('_')[0]
        commits[dev].append(int(m['t']))
    dropped_match=COUNTS_RE.search(analyzer)
    dropped=int(dropped_match['dropped']) if dropped_match else -1

    if not arm or not diag:
        raise SystemExit('ERROR: induced-NVS run records not found in serial log')
    target=int(arm['target'])
    completed=int(diag['completed'])
    successful=[e for e in events if e['set']=='ESP_OK' and e['commit']=='ESP_OK']
    guarded=[e for e in successful if int(e['ops'])>0]

    print(f'INDUCED_REQUESTED={diag["requested"]}')
    print(f'INDUCED_COMPLETED={completed}')
    print(f'INDUCED_SUCCESSFUL={len(successful)}')
    print(f'INDUCED_SUCCESSFUL_WITH_GUARD_OPS={len(guarded)}')
    if fdiag:
        print(f'FLASH_GUARD_OPERATIONS={fdiag["ops"]}')
        print(f'FLASH_GUARD_TOTAL_US={fdiag["total"]}')
        print(f'FLASH_GUARD_MAX_RETAINED_US={fdiag["max"]}')
    probe_ok = bool(successful) and len(guarded)==len(successful) and fdiag and int(fdiag['ops'])>0
    print(f'PROBE_POSITIVE_CONTROL={"PASS" if probe_ok else "FAIL"}')

    dev=args.device
    y=commits.get(dev, [])
    if len(y) < 3:
        raise SystemExit(f'ERROR: not enough {dev}_COMMIT edges')
    slope, intercept, residuals, rms=fit_line(y)
    print(f'ANALYZER_{dev}_BOUNDARIES={len(y)}')
    print(f'ANALYZER_{dev}_PERIOD_US={slope:.6f}')
    print(f'ANALYZER_{dev}_DETRENDED_RMS_US={rms:.3f}')
    print(f'ANALYZER_DROPPED={dropped}')

    near=[]
    for e in successful:
        begin=int(e['begin'])
        elapsed=begin-target
        boundary=int(round(elapsed/1_000_000.0))
        nominal=target+boundary*1_000_000
        phase=begin-nominal
        if 0 <= boundary < len(y) and abs(phase) <= args.near_boundary_us:
            near.append((abs(phase), boundary, phase, int(e['dur']), int(e['ops']), residuals[boundary], int(e['seq'])))
    near.sort()
    print(f'WRITES_WITHIN_{args.near_boundary_us}_US_OF_BOUNDARY={len(near)}')
    for _,boundary,phase,dur,ops,resid,seq in near[:20]:
        print(f'NEAR seq={seq} boundary={boundary} phase_us={phase:+d} write_us={dur} guard_ops={ops} analyzer_residual_us={resid:+.3f}')

    if near:
        max_abs=max(near, key=lambda r: abs(r[5]))
        print(f'MAX_NEAR_BOUNDARY_ANALYZER_RESIDUAL_US={max_abs[5]:+.3f} boundary={max_abs[1]} seq={max_abs[6]}')

if __name__=='__main__':
    main()
