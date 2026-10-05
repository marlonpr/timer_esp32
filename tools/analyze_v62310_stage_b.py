#!/usr/bin/env python3
import argparse, re
from pathlib import Path

LAT_RE = re.compile(
    r'CPU0_LATENCY_EVENT seq=(\d+) lateness_us=(\d+) expected_local_us=(-?\d+) actual_local_us=(-?\d+).*?interrupted_task=([^\s]+)')
COMMIT_RE = re.compile(
    r'CPU0_COMMIT_LATE seq=(\d+) boundary=(\d+).*?callback_lateness_us=(-?\d+).*?entry_to_marker_us=(-?\d+).*?interrupted_task=([^\s]+)')
TASK_CANARY_RE = re.compile(
    r'CPU0_LATENCY_CANARY requested=(\d+) completed=(\d+).*?critical_begin_us=(-?\d+) critical_end_us=(-?\d+) duration_us=(-?\d+).*?hold_us=(\d+) task=([^\s]+)')
ISR_CANARY_RE = re.compile(
    r'CPU0_ISR_CANARY requested=(\d+) completed=(\d+).*?isr_begin_us=(-?\d+) isr_end_us=(-?\d+) duration_us=(-?\d+).*?hold_us=(\d+) interrupted_task=([^\s]+)')
DIAG_RE = re.compile(
    r'CPU0_LATENCY_DIAG .*?period_us=(\d+) threshold_us=(\d+) guard_us=(-?\d+) '
    r'sample_callbacks=(\d+) wrong_core_callbacks=(\d+) guarded_samples=(\d+) events=(\d+) overflow=(\d+) '
    r'commit_late_events=(\d+) commit_overflow=(\d+) probe_gpio=(\d+) '
    r'probe_suppressed_events=(\d+) sampler_intr_level=(\d+) commit_intr_level=(\d+)')
RUN_RE = re.compile(r'CPU0_LATENCY_RUN_BEGIN .*?heartbeat_traffic=([^\s]+)')
EDGE_RE = re.compile(r'^ANZ\|EDGE\|[^|]+\|[^|]+\|\d+\|([^|]+)\|(\d+)\|([01])$', re.M)
COUNTS_RE = re.compile(r'^ANZ\|COUNTS\|.*$', re.M)


def parse_canary(m, task=False):
    if not m:
        return None
    if task:
        return dict(requested=int(m.group(1)), completed=int(m.group(2)), begin=int(m.group(3)),
                    end=int(m.group(4)), duration=int(m.group(5)), hold=int(m.group(6)), task=m.group(7))
    return dict(requested=int(m.group(1)), completed=int(m.group(2)), begin=int(m.group(3)),
                end=int(m.group(4)), duration=int(m.group(5)), hold=int(m.group(6)), task=m.group(7))


def overlaps(event, canary):
    if not canary:
        return False
    # A delayed sample is evidence for the blocker if its expected alarm was
    # inside the blocker interval, or if actual delivery is immediately after it.
    return (canary['begin'] <= event['expected'] <= canary['end'] or
            canary['begin'] <= event['actual'] <= canary['end'] + 100)


def parse_serial(path):
    text = Path(path).read_text(errors='replace')
    events = [dict(seq=int(a), lateness=int(b), expected=int(c), actual=int(d), task=e)
              for a,b,c,d,e in LAT_RE.findall(text)]
    commits = [dict(seq=int(a), boundary=int(b), lateness=int(c), entry_to_marker=int(d), task=e)
               for a,b,c,d,e in COMMIT_RE.findall(text)]
    task_canary = parse_canary(TASK_CANARY_RE.search(text), task=True)
    isr_canary = parse_canary(ISR_CANARY_RE.search(text))
    dm = DIAG_RE.search(text)
    diag = None if not dm else dict(period=int(dm.group(1)), threshold=int(dm.group(2)), guard=int(dm.group(3)),
        callbacks=int(dm.group(4)), wrong_core=int(dm.group(5)), guarded=int(dm.group(6)), events=int(dm.group(7)), overflow=int(dm.group(8)),
        commit_events=int(dm.group(9)), commit_overflow=int(dm.group(10)), probe_gpio=int(dm.group(11)),
        probe_suppressed=int(dm.group(12)), sampler_level=int(dm.group(13)), commit_level=int(dm.group(14)))
    rm = RUN_RE.search(text)
    heartbeat = rm.group(1) if rm else 'UNKNOWN'
    for e in events:
        if overlaps(e, task_canary): e['kind'] = 'TASK_CANARY'
        elif overlaps(e, isr_canary): e['kind'] = 'ISR_CANARY'
        else: e['kind'] = 'NATURAL'
    return text, events, commits, task_canary, isr_canary, diag, heartbeat


def parse_analyzer(path):
    text = Path(path).read_text(errors='replace')
    by = {}
    for name,t,l in EDGE_RE.findall(text):
        by.setdefault(name, []).append((int(t), int(l)))
    counts = COUNTS_RE.findall(text)
    return by, counts[-1] if counts else None


def pulses(edges):
    out=[]; start=None
    for t,l in edges:
        if l==1: start=t
        elif l==0 and start is not None and t>=start:
            out.append((start,t,t-start)); start=None
    return out


def linfit_residual(y, idx, half=5):
    xs=[]; ys=[]
    lo=max(0,idx-half); hi=min(len(y)-1,idx+half)
    for i in range(lo,hi+1):
        if i==idx: continue
        xs.append(float(i)); ys.append(float(y[i]))
    if len(xs)<2: return float('nan')
    mx=sum(xs)/len(xs); my=sum(ys)/len(ys)
    den=sum((x-mx)**2 for x in xs)
    if not den: return float('nan')
    b=sum((x-mx)*(yy-my) for x,yy in zip(xs,ys))/den
    return y[idx]-(my-b*mx+b*idx)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('esp01_serial')
    ap.add_argument('esp02_serial')
    ap.add_argument('analyzer')
    args=ap.parse_args()

    serials={}
    overall=True
    for dev,path in [('ESP01',args.esp01_serial),('ESP02',args.esp02_serial)]:
        serials[dev]=parse_serial(path)
        _,events,commits,task_canary,isr_canary,diag,hb=serials[dev]
        natural=[e for e in events if e['kind']=='NATURAL']
        task_events=[e for e in events if e['kind']=='TASK_CANARY']
        isr_events=[e for e in events if e['kind']=='ISR_CANARY']
        print(f'{dev}_HEARTBEAT_TRAFFIC={hb}')
        print(f'{dev}_LATENCY_EVENTS={len(events)}')
        print(f'{dev}_NATURAL_LATENCY_EVENTS={len(natural)}')
        print(f'{dev}_COMMIT_LATE_EVENTS={len(commits)}')
        if diag:
            print(f'{dev}_MONITOR_PERIOD_US={diag["period"]}')
            print(f'{dev}_MONITOR_THRESHOLD_US={diag["threshold"]}')
            print(f'{dev}_GUARANTEED_DETECTION_MASK_US={diag["period"] + diag["threshold"]}')
            print(f'{dev}_SAMPLER_INTR_LEVEL={diag["sampler_level"]}')
            print(f'{dev}_COMMIT_INTR_LEVEL={diag["commit_level"]}')
            print(f'{dev}_PROBE_SUPPRESSED_EVENTS={diag["probe_suppressed"]}')
            print(f'{dev}_MONITOR_OVERFLOW={diag["overflow"]}')
            print(f'{dev}_WRONG_CORE_CALLBACKS={diag["wrong_core"]}')
        task_ok=bool(task_canary and task_canary['requested']==1 and task_canary['completed']==1 and
                     500 <= task_canary['duration'] <= 850 and any(e['task']=='lat_canary' for e in task_events))
        isr_ok=bool(isr_canary and isr_canary['requested']==1 and isr_canary['completed']==1 and
                    400 <= isr_canary['duration'] <= 800 and len(isr_events)>=1)
        level_ok=bool(diag and diag['sampler_level']==diag['commit_level'] and diag['wrong_core']==0)
        workload_ok=(hb=='suppressed')
        ring_ok=bool(diag and diag['overflow']==0 and diag['commit_overflow']==0)
        print(f'{dev}_TASK_CANARY={"PASS" if task_ok else "FAIL"}')
        print(f'{dev}_ISR_CANARY={"PASS" if isr_ok else "FAIL"}')
        print(f'{dev}_LEVEL_MATCH={"PASS" if level_ok else "FAIL"}')
        print(f'{dev}_HISTORICAL_WORKLOAD_MATCH={"PASS" if workload_ok else "FAIL"}')
        print(f'{dev}_RING_HEALTH={"PASS" if ring_ok else "FAIL"}')
        overall &= task_ok and isr_ok and level_ok and workload_ok and ring_ok
        for e in events:
            print(f'{dev}_LATENCY kind={e["kind"]} seq={e["seq"]} lateness_us={e["lateness"]} task={e["task"]}')
        for e in commits:
            print(f'{dev}_COMMIT_LATE boundary={e["boundary"]} lateness_us={e["lateness"]} entry_to_marker_us={e["entry_to_marker"]} task={e["task"]}')

    an,counts=parse_analyzer(args.analyzer)
    c1=[t for t,_ in an.get('ESP01_COMMIT',[])]
    c2=[t for t,_ in an.get('ESP02_COMMIT',[])]
    p1=pulses(an.get('ESP01_SQW',[])); p2=pulses(an.get('ESP02_SQW',[]))
    print(f'ANALYZER_ESP01_COMMIT={len(c1)}')
    print(f'ANALYZER_ESP02_COMMIT={len(c2)}')
    print(f'ANALYZER_ESP01_PROBE_PULSES={len(p1)}')
    print(f'ANALYZER_ESP02_PROBE_PULSES={len(p2)}')
    if counts: print(f'ANALYZER_COUNTS_LINE={counts}')
    for dev, commits in [('ESP01',c1),('ESP02',c2)]:
        for e in serials[dev][2]:
            b=e['boundary']
            if 0<=b<len(commits):
                print(f'{dev}_ANALYZER_COMMIT_LATE boundary={b} local_fit_residual_us={linfit_residual(commits,b):+.3f}')

    # Critical inference rule for Stage B: because sampler and COMMIT are same
    # interrupt level/core and the sampler period+threshold is 300 us, a >=358 us
    # pre-entry COMMIT stall with no nearby latency event is incompatible with a
    # continuous CPU0 mask/same-level ISR blocker of that magnitude.
    for dev in ('ESP01','ESP02'):
        events=serials[dev][1]
        for c in serials[dev][2]:
            if c['lateness'] >= 300 and c['entry_to_marker'] <= 5:
                print(f'{dev}_PREENTRY_STALL_REQUIRES_MONITOR_CORRELATION boundary={c["boundary"]} lateness_us={c["lateness"]}')

    print(f'STAGE_B_MONITOR_VALIDATION={"PASS" if overall else "FAIL"}')

if __name__=='__main__': main()
