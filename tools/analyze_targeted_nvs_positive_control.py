#!/usr/bin/env python3
"""Analyze v6.23.7 targeted NVS/cache-window positive-control logs."""
from __future__ import annotations
import argparse, math, re
from pathlib import Path

EVENT_RE = re.compile(
    r'INDUCED_NVS_EVENT .*?sequence=(?P<seq>\d+) target_boundary=(?P<boundary>\d+) '
    r'target_local_us=(?P<target>-?\d+) wake_local_us=(?P<wake>-?\d+) '
    r'set_begin_local_us=(?P<setbegin>-?\d+) set_end_local_us=(?P<setend>-?\d+) '
    r'commit_begin_local_us=(?P<begin>-?\d+) commit_begin_minus_target_us=(?P<phase>-?\d+) '
    r'commit_end_local_us=(?P<end>-?\d+) commit_duration_us=(?P<dur>-?\d+) value=(?P<value>\d+) '
    r'flash_operations=(?P<ops>\d+) set_err=(?P<set>\S+) commit_err=(?P<commit>\S+)')
DIAG_RE = re.compile(
    r'INDUCED_NVS_DIAG .*?requested=(?P<requested>\d+) completed=(?P<completed>\d+).*?'
    r'timer_start_err=(?P<start>\S+) timer_stop_err=(?P<stop>\S+)')
FLASH_RE = re.compile(
    r'FLASH_OS_DIAG .*?operations=(?P<ops>\d+) .*?total_us=(?P<total>\d+) '
    r'.*?max_retained_us=(?P<max>\d+) .*?flash_counters_valid=(?P<cvalid>\d+) '
    r'write_count_delta=(?P<wcount>\d+) write_time_us_delta=(?P<wtime>\d+) '
    r'write_bytes_delta=(?P<wbytes>\d+) erase_count_delta=(?P<ecount>\d+) '
    r'erase_time_us_delta=(?P<etime>\d+)')
WINDOW_RE = re.compile(r'FLASH_OS_EVENT .*?start_local_us=(?P<start>-?\d+) duration_us=(?P<dur>\d+)')
EDGE_RE = re.compile(r'ANZ\|EDGE\|[^|]*\|[^|]*\|[^|]*\|(?P<name>ESP0[12]_COMMIT)\|(?P<t>\d+)\|')
COUNTS_RE = re.compile(r'ANZ\|COUNTS\|.*?dropped=(?P<dropped>\d+)')


def fit_line(y: list[int]):
    n=len(y); xb=(n-1)/2.0; yb=sum(y)/n
    sxx=sum((i-xb)**2 for i in range(n))
    slope=sum((i-xb)*(v-yb) for i,v in enumerate(y))/sxx
    intercept=yb-slope*xb
    res=[v-(intercept+slope*i) for i,v in enumerate(y)]
    rms=math.sqrt(sum(r*r for r in res)/n)
    return slope, intercept, res, rms


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('serial_log')
    ap.add_argument('analyzer_log')
    ap.add_argument('--device', default='ESP02')
    args=ap.parse_args()
    serial=Path(args.serial_log).read_text(errors='replace')
    analyzer=Path(args.analyzer_log).read_text(errors='replace')

    diag=DIAG_RE.search(serial)
    fdiag=FLASH_RE.search(serial)
    events=[m.groupdict() for m in EVENT_RE.finditer(serial)]
    windows=[(int(m['start']), int(m['dur'])) for m in WINDOW_RE.finditer(serial)]
    if not diag or not fdiag or not events:
        raise SystemExit('ERROR: v6.23.7 targeted positive-control records not found')

    commits={'ESP01':[], 'ESP02':[]}
    for m in EDGE_RE.finditer(analyzer):
        commits[m['name'].split('_')[0]].append(int(m['t']))
    dropped_match=COUNTS_RE.search(analyzer)
    dropped=int(dropped_match['dropped']) if dropped_match else -1

    dev=args.device
    y=commits.get(dev, [])
    if len(y)<3:
        raise SystemExit(f'ERROR: not enough {dev}_COMMIT edges')
    period, intercept, residuals, rms=fit_line(y)

    successful=[e for e in events if e['set']=='ESP_OK' and e['commit']=='ESP_OK']
    guarded=[e for e in successful if int(e['ops'])>0]
    call_overlaps=[]
    flash_overlaps=[]
    for e in successful:
        b=int(e['boundary']); target=int(e['target']); begin=int(e['begin']); end=int(e['end'])
        if begin <= target <= end:
            call_overlaps.append(e)
        if any(ws <= target <= ws+wd for ws,wd in windows):
            flash_overlaps.append(e)

    probe_ok=(len(guarded)==len(successful) and len(successful)>0 and
              int(fdiag['ops'])>0 and int(fdiag['cvalid'])==1 and int(fdiag['wcount'])>0)

    print(f'INDUCED_REQUESTED={diag["requested"]}')
    print(f'INDUCED_COMPLETED={diag["completed"]}')
    print(f'INDUCED_SUCCESSFUL={len(successful)}')
    print(f'INDUCED_SUCCESSFUL_WITH_FLASH_OPS={len(guarded)}')
    print(f'FLASH_OS_OPERATIONS={fdiag["ops"]}')
    print(f'FLASH_OS_TOTAL_US={fdiag["total"]}')
    print(f'FLASH_OS_MAX_RETAINED_US={fdiag["max"]}')
    print(f'FLASH_COUNTER_WRITE_COUNT_DELTA={fdiag["wcount"]}')
    print(f'FLASH_COUNTER_WRITE_TIME_US_DELTA={fdiag["wtime"]}')
    print(f'FLASH_COUNTER_WRITE_BYTES_DELTA={fdiag["wbytes"]}')
    print(f'PROBE_POSITIVE_CONTROL={"PASS" if probe_ok else "FAIL"}')
    print(f'ANALYZER_{dev}_BOUNDARIES={len(y)}')
    print(f'ANALYZER_{dev}_PERIOD_US={period:.6f}')
    print(f'ANALYZER_{dev}_DETRENDED_RMS_US={rms:.3f}')
    print(f'ANALYZER_DROPPED={dropped}')
    print(f'NVS_CALL_BOUNDARY_OVERLAPS={len(call_overlaps)}')
    print(f'FLASH_WINDOW_BOUNDARY_OVERLAPS={len(flash_overlaps)}')

    for e in successful:
        b=int(e['boundary']); phase=int(e['phase']); dur=int(e['dur']); ops=int(e['ops'])
        resid=residuals[b] if 0 <= b < len(residuals) else float('nan')
        call_overlap=int(int(e['begin']) <= int(e['target']) <= int(e['end']))
        fover=int(e in flash_overlaps)
        print(f'TARGET seq={e["seq"]} boundary={b} begin_minus_target_us={phase:+d} '
              f'write_us={dur} flash_ops={ops} call_overlap={call_overlap} '
              f'flash_overlap={fover} analyzer_residual_us={resid:+.3f}')

if __name__=='__main__':
    main()
