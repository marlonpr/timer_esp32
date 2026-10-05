#!/usr/bin/env python3
"""Analyze the v6.23.8 deterministic cache/critical-section mechanism test.

Analyzer_v8 channel 4 is reused for ESP02 GPIO32 and still appears in the log as
ESP02_SQW. The script treats it as ESP02_BLOCK_PROBE for this diagnostic run.
"""
from __future__ import annotations
import argparse
import math
import re
from pathlib import Path

MECH_RE = re.compile(
    r'TIMING_MECHANISM_EVENT .*?sequence=(?P<seq>\d+) kind=(?P<kind>\S+) '
    r'boundary=(?P<boundary>\d+) core=(?P<core>\d+) target_local_us=(?P<target>-?\d+) '
    r'requested_start_local_us=(?P<requested>-?\d+) worker_awake_us=(?P<awake>-?\d+) '
    r'operation_call_begin_us=(?P<call_begin>-?\d+) call_begin_minus_target_us=(?P<call_phase>-?\d+) '
    r'protected_begin_us=(?P<begin>-?\d+) protected_begin_minus_target_us=(?P<begin_phase>-?\d+) '
    r'protected_end_us=(?P<end>-?\d+) protected_end_minus_target_us=(?P<end_phase>-?\d+) '
    r'protected_duration_us=(?P<duration>-?\d+) operation_call_end_us=(?P<call_end>-?\d+) '
    r'requested_hold_us=(?P<hold>\d+)')

FLASH_RE = re.compile(
    r'FLASH_OS_EVENT .*?sequence=(?P<seq>\d+) core=(?P<core>\d+) '
    r'start_hook_enter_us=(?P<hook>-?\d+) cache_off_begin_us=(?P<begin>-?\d+) '
    r'start_wait_us=(?P<wait>-?\d+) cache_off_end_us=(?P<end>-?\d+) '
    r'cache_off_duration_us=(?P<duration>-?\d+) end_hook_return_us=(?P<return>-?\d+) '
    r'end_restore_us=(?P<restore>-?\d+)')

EDGE_RE = re.compile(
    r'ANZ\|EDGE\|[^|]*\|[^|]*\|(?P<ch>\d+)\|(?P<name>[^|]+)\|(?P<t>\d+)\|(?P<level>[01])')
COUNTS_RE = re.compile(r'ANZ\|COUNTS\|.*?dropped=(?P<dropped>\d+)')
ISR_RE = re.compile(r'ISR_PUBLISH boundary=(?P<boundary>\d+) .*?callback_lateness_us=(?P<late>-?\d+)')
VIS_RE = re.compile(r'Visual countdown finished: .*?worst_isr_publish_marker_lateness_us=(?P<worst>-?\d+)')

TARGETS = {30: 'CACHE_OFF', 60: 'CPU0_CRITICAL', 90: 'CPU1_CRITICAL'}


def fit_line_excluding(y: list[int], excluded: set[int]):
    pts=[(i,v) for i,v in enumerate(y) if i not in excluded]
    n=len(pts)
    xb=sum(i for i,_ in pts)/n
    yb=sum(v for _,v in pts)/n
    sxx=sum((i-xb)**2 for i,_ in pts)
    slope=sum((i-xb)*(v-yb) for i,v in pts)/sxx
    intercept=yb-slope*xb
    res=[v-(intercept+slope*i) for i,v in enumerate(y)]
    rms=math.sqrt(sum(res[i]*res[i] for i,_ in pts)/n)
    return slope, intercept, res, rms


def pair_pulses(edges: list[tuple[int,int]]):
    pulses=[]
    open_t=None
    for t,level in edges:
        if level==1:
            open_t=t
        elif level==0 and open_t is not None and t>=open_t:
            pulses.append((open_t,t))
            open_t=None
    return pulses


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('esp02_serial')
    ap.add_argument('analyzer_log')
    ap.add_argument('--probe-name', default='ESP02_SQW',
                    help='Analyzer_v8 label reused for ESP02 GPIO32 (default ESP02_SQW)')
    args=ap.parse_args()

    serial=Path(args.esp02_serial).read_text(errors='replace')
    analyzer=Path(args.analyzer_log).read_text(errors='replace')

    events={int(m['boundary']):m.groupdict() for m in MECH_RE.finditer(serial)}
    flash=[m.groupdict() for m in FLASH_RE.finditer(serial)]
    isr={int(m['boundary']):int(m['late']) for m in ISR_RE.finditer(serial)}
    vis=VIS_RE.search(serial)

    commits={'ESP01':[], 'ESP02':[]}
    probe=[]
    for m in EDGE_RE.finditer(analyzer):
        name=m['name']; t=int(m['t']); level=int(m['level'])
        if name=='ESP01_COMMIT': commits['ESP01'].append(t)
        elif name=='ESP02_COMMIT': commits['ESP02'].append(t)
        elif name==args.probe_name: probe.append((t,level))

    if len(commits['ESP02']) < 91:
        raise SystemExit(f'ERROR: only {len(commits["ESP02"])} ESP02 COMMIT edges')
    if len(events) != 3:
        raise SystemExit(f'ERROR: expected 3 TIMING_MECHANISM_EVENT records, found {len(events)}')

    slope, intercept, residuals, rms = fit_line_excluding(commits['ESP02'], set(TARGETS))
    pulses=pair_pulses(probe)
    dropped_match=COUNTS_RE.search(analyzer)
    dropped=int(dropped_match['dropped']) if dropped_match else -1

    # With exactly three deliberate pulses, order maps directly to 30/60/90. If
    # incidental flash windows add pulses, match each target to the pulse nearest
    # the fitted boundary prediction.
    used=set(); matched={}
    for b in sorted(TARGETS):
        pred=intercept+slope*b
        candidates=[]
        for i,(rise,fall) in enumerate(pulses):
            if i in used: continue
            center=(rise+fall)/2.0
            candidates.append((abs(center-pred),i,rise,fall))
        if candidates:
            _,i,rise,fall=min(candidates)
            used.add(i); matched[b]=(rise,fall)

    print(f'ANALYZER_ESP02_BOUNDARIES={len(commits["ESP02"])}')
    print(f'ANALYZER_ESP02_BASELINE_PERIOD_US={slope:.6f}')
    print(f'ANALYZER_ESP02_BASELINE_RMS_US={rms:.3f}')
    print(f'ANALYZER_PROBE_EDGES={len(probe)}')
    print(f'ANALYZER_PROBE_PULSES={len(pulses)}')
    print(f'ANALYZER_DROPPED={dropped}')
    print(f'FLASH_OS_EVENTS_RETAINED={len(flash)}')
    if flash:
        print(f'FLASH_OS_MAX_CACHE_OFF_US={max(int(x["duration"]) for x in flash)}')
        print(f'FLASH_OS_MAX_START_WAIT_US={max(int(x["wait"]) for x in flash)}')
    if vis:
        print(f'FIRMWARE_WORST_PUBLISH_LATENESS_US={vis["worst"]}')

    verdicts={}
    for b,kind in TARGETS.items():
        e=events[b]
        if e['kind'] != kind:
            raise SystemExit(f'ERROR: boundary {b} expected {kind}, saw {e["kind"]}')
        pred=intercept+slope*b
        actual=commits['ESP02'][b]
        resid=actual-pred
        pulse=matched.get(b)
        if pulse:
            rise,fall=pulse
            rise_rel=rise-pred; fall_rel=fall-pred
            predicted_inside=rise <= pred <= fall
            actual_inside=rise <= actual <= fall
            after_fall=actual-fall
            pulse_us=fall-rise
        else:
            rise_rel=fall_rel=after_fall=float('nan')
            predicted_inside=actual_inside=False
            pulse_us=float('nan')
        fw=isr.get(b)
        print(
            f'TARGET boundary={b} kind={kind} '
            f'firmware_protected_begin_minus_target_us={int(e["begin_phase"]):+d} '
            f'firmware_protected_end_minus_target_us={int(e["end_phase"]):+d} '
            f'firmware_protected_duration_us={int(e["duration"])} '
            f'analyzer_probe_rise_minus_grid_us={rise_rel:+.3f} '
            f'analyzer_probe_fall_minus_grid_us={fall_rel:+.3f} '
            f'analyzer_probe_width_us={pulse_us:.3f} '
            f'grid_inside_probe={int(predicted_inside)} '
            f'commit_inside_probe={int(actual_inside)} '
            f'analyzer_commit_residual_us={resid:+.3f} '
            f'commit_minus_probe_fall_us={after_fall:+.3f} '
            f'firmware_callback_lateness_us={fw if fw is not None else "NA"}'
        )

        if kind=='CACHE_OFF':
            verdicts[b]=predicted_inside and actual_inside and abs(resid) <= 50
        elif kind=='CPU0_CRITICAL':
            verdicts[b]=predicted_inside and (not actual_inside) and resid >= 100 and after_fall >= 0
        else:
            verdicts[b]=predicted_inside and actual_inside and abs(resid) <= 50

    print(f'CACHE_OFF_IRAM_PATH_TEST={"PASS" if verdicts.get(30) else "FAIL"}')
    print(f'CPU0_MASK_POSITIVE_CONTROL={"PASS" if verdicts.get(60) else "FAIL"}')
    print(f'CPU1_LOCALITY_CONTROL={"PASS" if verdicts.get(90) else "FAIL"}')
    overall=all(verdicts.values()) and dropped in (-1,0)
    print(f'RESULT={"PASS" if overall else "FAIL"}')

if __name__=='__main__':
    main()
