#!/usr/bin/env python3
"""Summarize v6.14 ISR_PUBLISH records and optionally correlate analyzer v4 edges."""
import argparse, re, statistics

ISR_RE = re.compile(r"ISR_PUBLISH boundary=(\d+).*?target_local_us=(-?\d+).*?arm_us=(-?\d+).*?callback_entry_us=(-?\d+).*?publish_marker_begin_us=(-?\d+).*?publish_marker_end_us=(-?\d+).*?callback_exit_us=(-?\d+).*?post_local_us=(-?\d+).*?prepared_sequence=(\d+).*?published_sequence=(\d+).*?published=(\d+).*?marker_level=(\d+).*?frame_not_ready_count=(\d+)")
EDGE_RE = re.compile(r"ANZ\|EDGE\|\d+\|\d+\|\d+\|([^|]+)\|(\d+)(?:\|(\d+))?")

def pct(xs,p):
    if not xs: return float('nan')
    ys=sorted(xs); pos=(len(ys)-1)*p; lo=int(pos); hi=min(lo+1,len(ys)-1); f=pos-lo
    return ys[lo]*(1-f)+ys[hi]*f

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('device_log')
    ap.add_argument('--analyzer')
    ap.add_argument('--device', default='ESP01')
    a=ap.parse_args()
    txt=open(a.device_log,errors='replace').read()
    rows=[]
    for m in ISR_RE.finditer(txt):
        b,target,arm,cb,beg,end,exit_,post,prep,pubseq,pub,level,nr=map(int,m.groups())
        rows.append(dict(boundary=b,target=target,arm=arm,cb=cb,beg=beg,end=end,exit=exit_,post=post,prep=prep,pubseq=pubseq,pub=pub,level=level,nr=nr))
    if not rows:
        raise SystemExit('No ISR_PUBLISH records found')
    late=[r['end']-r['target'] for r in rows if r['pub']]
    cb_late=[r['cb']-r['target'] for r in rows if r['pub']]
    pm=[r['end']-r['beg'] for r in rows if r['pub']]
    print(f"records={len(rows)} published={sum(r['pub'] for r in rows)} frame_not_ready_final={max(r['nr'] for r in rows)}")
    print(f"callback_lateness_us median={statistics.median(cb_late):.1f} p95={pct(cb_late,.95):.1f} max={max(cb_late):.1f}")
    print(f"marker_end_lateness_us median={statistics.median(late):.1f} p95={pct(late,.95):.1f} max={max(late):.1f}")
    print(f"publish+marker_duration_us median={statistics.median(pm):.1f} p95={pct(pm,.95):.1f} max={max(pm):.1f}")
    print(f"post_minus_target_us max_abs={max(abs(r['post']-r['target']) for r in rows):.1f}")
    for r in rows:
        print(f"b{r['boundary']:02d} cb={r['cb']-r['target']:+d}us marker={r['end']-r['target']:+d}us op={r['end']-r['beg']}us arm_margin={r['target']-r['arm']}us ready={r['pub']} level={r['level']}")
    if a.analyzer:
        edges=[]
        atxt=open(a.analyzer,errors='replace').read()
        for m in EDGE_RE.finditer(atxt):
            dev,ts,level=m.groups()
            if dev==a.device:
                edges.append((int(ts), None if level is None else int(level)))
        # First edge is START, following edges map to boundary 1..N.
        if len(edges) >= len(rows)+1:
            diffs=[]
            for r,(ts,lvl) in zip(rows,edges[1:]):
                diffs.append(ts-r['end'])
            # Remove offset and linear drift only; level term can be examined by split means.
            n=len(diffs); x=list(range(n)); xm=sum(x)/n; ym=sum(diffs)/n
            den=sum((q-xm)**2 for q in x)
            slope=sum((q-xm)*(y-ym) for q,y in zip(x,diffs))/den if den else 0
            detr=[y-(ym+slope*(q-xm)) for q,y in zip(x,diffs)]
            print(f"analyzer_minus_marker detrended_rms_us={(sum(v*v for v in detr)/n)**0.5:.3f} max_abs_us={max(abs(v) for v in detr):.3f} slope_us_per_boundary={slope:.6f}")
            levels=[e[1] for e in edges[1:len(rows)+1]]
            if all(v is not None for v in levels):
                hi=[d for d,l in zip(detr,levels) if l==1]; lo=[d for d,l in zip(detr,levels) if l==0]
                if hi and lo:
                    print(f"level_effect_high_minus_low_us={statistics.mean(hi)-statistics.mean(lo):.3f}")
        else:
            print(f"analyzer correlation skipped: need >= {len(rows)+1} {a.device} edges, found {len(edges)}")
if __name__=='__main__': main()
