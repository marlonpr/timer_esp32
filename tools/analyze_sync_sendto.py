#!/usr/bin/env python3
"""Summarize buffered v6.14 SYNC_REPLY_TRACE device-side send timing."""
import argparse,re,statistics
R=re.compile(r"SYNC_REPLY_TRACE sample=(\d+).*?t2_local_us=(-?\d+).*?t3_local_us=(-?\d+).*?format_done_us=(-?\d+).*?t3_to_sendto_entry_us=(-?\d+).*?sendto_entry_us=(-?\d+).*?sendto_return_us=(-?\d+).*?sendto_duration_us=(-?\d+).*?t3_to_sendto_return_us=(-?\d+).*?requested_delay_us=(\d+).*?reported_hold_us=(\d+).*?sent=(\d+)")
def stats(name,x):
    if x: print(f"{name}: n={len(x)} min={min(x):.1f} median={statistics.median(x):.1f} max={max(x):.1f} us")
def main():
    ap=argparse.ArgumentParser(); ap.add_argument('log'); a=ap.parse_args(); txt=open(a.log,errors='replace').read(); rows=[]
    for m in R.finditer(txt): rows.append(tuple(map(int,m.groups())))
    if not rows: raise SystemExit('No SYNC_REPLY_TRACE samples found')
    normal=[r for r in rows if r[9]==0 and r[11]==1]
    print(f"samples={len(rows)} normal_sent={len(normal)}")
    stats('t3_to_sendto_entry',[r[4] for r in normal])
    stats('sendto_duration',[r[7] for r in normal])
    stats('t3_to_sendto_return',[r[8] for r in normal])
    for r in rows:
        print(f"sample={r[0]} t2_to_t3={r[2]-r[1]:+d}us t3_to_entry={r[4]:+d}us sendto={r[7]:+d}us t3_to_return={r[8]:+d}us requested={r[9]} hold={r[10]} sent={r[11]}")
if __name__=='__main__': main()
