#!/usr/bin/env python3
import argparse, math, re
from pathlib import Path

LAT_RE = re.compile(r'CPU0_LATENCY_EVENT seq=(\d+) lateness_us=(\d+).*?interrupted_task=([^\s]+)')
COMMIT_RE = re.compile(r'CPU0_COMMIT_LATE seq=(\d+) boundary=(\d+).*?callback_lateness_us=(-?\d+).*?entry_to_marker_us=(-?\d+).*?interrupted_task=([^\s]+)')
CANARY_RE = re.compile(r'CPU0_LATENCY_CANARY requested=(\d+) completed=(\d+).*?duration_us=(-?\d+).*?hold_us=(\d+) task=([^\s]+)')
DIAG_RE = re.compile(r'CPU0_LATENCY_DIAG .*?period_us=(\d+) threshold_us=(\d+) guard_us=(-?\d+)(?: sample_callbacks=(\d+) guarded_samples=(\d+))?.*?events=(\d+) overflow=(\d+) commit_late_events=(\d+) commit_overflow=(\d+) probe_gpio=(\d+)')
EDGE_RE = re.compile(r'^ANZ\|EDGE\|[^|]+\|[^|]+\|\d+\|([^|]+)\|(\d+)\|([01])$', re.M)

def parse_serial(path):
    text = Path(path).read_text(errors='replace')
    lat = [dict(seq=int(a), lateness=int(b), task=c) for a,b,c in LAT_RE.findall(text)]
    commit = [dict(seq=int(a), boundary=int(b), lateness=int(c), entry_to_marker=int(d), task=e)
              for a,b,c,d,e in COMMIT_RE.findall(text)]
    canary_m = CANARY_RE.search(text)
    canary = None if not canary_m else dict(requested=int(canary_m.group(1)), completed=int(canary_m.group(2)), duration=int(canary_m.group(3)), hold=int(canary_m.group(4)), task=canary_m.group(5))
    diag_m = DIAG_RE.search(text)
    diag = None if not diag_m else dict(
        period=int(diag_m.group(1)),
        threshold=int(diag_m.group(2)),
        guard=int(diag_m.group(3)),
        sample_callbacks=int(diag_m.group(4)) if diag_m.group(4) else None,
        guarded_samples=int(diag_m.group(5)) if diag_m.group(5) else None,
        events=int(diag_m.group(6)),
        overflow=int(diag_m.group(7)),
        commit_events=int(diag_m.group(8)),
        commit_overflow=int(diag_m.group(9)),
        probe_gpio=int(diag_m.group(10)))
    heartbeats = text.count('STATUS_TX reason=HEARTBEAT')
    return text, lat, commit, canary, diag, heartbeats

def parse_analyzer(path):
    text = Path(path).read_text(errors='replace')
    by = {}
    for name,t,l in EDGE_RE.findall(text):
        by.setdefault(name, []).append((int(t), int(l)))
    return by

def pulses(edges):
    out=[]; start=None
    for t,l in edges:
        if l==1:
            start=t
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
    if den==0: return float('nan')
    b=sum((x-mx)*(yy-my) for x,yy in zip(xs,ys))/den
    a=my-b*mx
    return y[idx]-(a+b*idx)

def slope_ppm(a,b,offset):
    n=min(len(a),len(b))
    if n<2: return float('nan')
    x=[(a[i]-a[0])/1e6 for i in range(n)]
    d=[(b[i]-a[i])-offset for i in range(n)]
    mx=sum(x)/n; md=sum(d)/n
    den=sum((xx-mx)**2 for xx in x)
    return sum((xx-mx)*(dd-md) for xx,dd in zip(x,d))/den if den else float('nan')

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('esp01_serial')
    ap.add_argument('esp02_serial')
    ap.add_argument('analyzer')
    ap.add_argument('--esp02-offset-us',type=float,default=50.0)
    args=ap.parse_args()

    serials={}
    for dev,path in [('ESP01',args.esp01_serial),('ESP02',args.esp02_serial)]:
        serials[dev]=parse_serial(path)
        _,lat,commit,canary,diag,hb=serials[dev]
        print(f'{dev}_LATENCY_EVENTS={len(lat)}')
        print(f'{dev}_NATURAL_LATENCY_EVENTS={sum(1 for e in lat if e["task"]!="lat_canary")}')
        print(f'{dev}_COMMIT_LATE_EVENTS={len(commit)}')
        print(f'{dev}_HEARTBEAT_TRACE_RECORDS={hb}')
        if diag:
            print(f'{dev}_MONITOR_PERIOD_US={diag["period"]}')
            print(f'{dev}_MONITOR_THRESHOLD_US={diag["threshold"]}')
            print(f'{dev}_GUARANTEED_DETECTION_MASK_US={diag["period"] + diag["threshold"]}')
            if diag['sample_callbacks'] is not None:
                print(f'{dev}_SAMPLE_CALLBACKS={diag["sample_callbacks"]}')
                print(f'{dev}_GUARDED_SAMPLES={diag["guarded_samples"]}')
            print(f'{dev}_MONITOR_OVERFLOW={diag["overflow"]}')
        canary_ok=bool(canary and canary['requested']==1 and canary['completed']==1 and 500<=canary['duration']<=800 and any(e['task']=='lat_canary' and e['lateness']>=300 for e in lat))
        print(f'{dev}_CANARY={"PASS" if canary_ok else "FAIL"}')
        if canary:
            print(f'{dev}_CANARY_DURATION_US={canary["duration"]}')
        for e in lat:
            print(f'{dev}_LATENCY seq={e["seq"]} lateness_us={e["lateness"]} task={e["task"]}')
        for e in commit:
            print(f'{dev}_COMMIT_LATE boundary={e["boundary"]} lateness_us={e["lateness"]} entry_to_marker_us={e["entry_to_marker"]} task={e["task"]}')

    an=parse_analyzer(args.analyzer)
    c1=[t for t,_ in an.get('ESP01_COMMIT',[])]
    c2=[t for t,_ in an.get('ESP02_COMMIT',[])]
    p1=pulses(an.get('ESP01_SQW',[]))
    p2=pulses(an.get('ESP02_SQW',[]))
    print(f'ANALYZER_ESP01_COMMIT={len(c1)}')
    print(f'ANALYZER_ESP02_COMMIT={len(c2)}')
    print(f'ANALYZER_ESP01_PROBE_PULSES={len(p1)}')
    print(f'ANALYZER_ESP02_PROBE_PULSES={len(p2)}')
    if c1 and c2:
        n=min(len(c1),len(c2))
        raw=[c2[i]-c1[i] for i in range(n)]
        print(f'ANALYZER_MIN_ABS_RAW_PAIR_SEPARATION_US={min(abs(x) for x in raw):.3f}')
        print(f'ANALYZER_PAIR_SLOPE_AFTER_{args.esp02_offset_us:.0f}US_OFFSET_PPM={slope_ppm(c1,c2,args.esp02_offset_us):+.6f}')
        print(f'ANALYZER_CLOSE_EDGE_GUARD={"PASS" if min(abs(x) for x in raw)>=20 else "WARN"}')
    for dev, commits in [('ESP01',c1),('ESP02',c2)]:
        for e in serials[dev][2]:
            b=e['boundary']
            if 0<=b<len(commits):
                print(f'{dev}_ANALYZER_COMMIT_LATE boundary={b} local_fit_residual_us={linfit_residual(commits,b):+.3f}')
    for dev,ps,commits in [('ESP01',p1,c1),('ESP02',p2,c2)]:
        if commits:
            for i,(a,b,d) in enumerate(ps,1):
                rel=(a-commits[0])/1e6
                print(f'{dev}_PROBE pulse={i} duration_us={d} seconds_from_boundary0={rel:.6f}')

    canary_all=all(serials[d][3] and serials[d][3]['completed']==1 and any(e['task']=='lat_canary' and e['lateness']>=300 for e in serials[d][1]) for d in ('ESP01','ESP02'))
    overflow_ok=all((serials[d][4] is not None and serials[d][4]['overflow']==0 and serials[d][4]['commit_overflow']==0) for d in ('ESP01','ESP02'))
    print(f'MONITOR_POSITIVE_CONTROL={"PASS" if canary_all else "FAIL"}')
    print(f'MONITOR_RING_HEALTH={"PASS" if overflow_ok else "FAIL"}')

if __name__=='__main__': main()
