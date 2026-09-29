#!/usr/bin/env python3
"""Summarize v6.13 display wake/commit diagnostics and STATUS timing traces."""
from __future__ import annotations
import argparse, csv, re
from pathlib import Path

KV_RE = re.compile(r"([A-Za-z_]+)=([^ ]+)")

def kv(line: str):
    return {k:v for k,v in KV_RE.findall(line)}

def i(d,k,default=0):
    try:return int(d.get(k,default))
    except:return default

def parse_log(path: Path):
    boundaries=[]; delays=[]; tasks=[]; status=[]; tx=[]; rx=[]
    for raw in path.read_text(errors='replace').splitlines():
        line=raw.strip()
        if 'BOUNDARY_TRACE boundary=' in line:
            d=kv(line); boundaries.append(d)
        elif 'COMMIT_DELAY boundary=' in line and 'COMMIT_DELAY_TASK' not in line:
            delays.append(kv(line))
        elif 'COMMIT_DELAY_TASK boundary=' in line:
            tasks.append(kv(line))
        elif 'STATUS_PATH command=' in line:
            status.append(kv(line))
        elif 'STATUS_TX reason=' in line:
            tx.append(kv(line))
        elif 'RX_NEAR_BOUNDARY device=' in line:
            rx.append(kv(line))
    return boundaries,delays,tasks,status,tx,rx

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('device_log', type=Path)
    ap.add_argument('--controller-csv', type=Path)
    args=ap.parse_args()
    boundaries,delays,tasks,status,tx,rx=parse_log(args.device_log)
    print(f'boundaries={len(boundaries)} late_commit_events={len(delays)} status_requests={len(status)} status_tx={len(tx)} rx_near={len(rx)}')
    if boundaries:
        print('\nBoundary timing:')
        print(' b  entry_rel  snap  request_late  flip_call  commit_late  toggle_late')
        for d in boundaries:
            print(f"{i(d,'boundary'):2d} {i(d,'precision_entry_lateness_us'):10d} {i(d,'runtime_snapshot_cost_us'):5d} {i(d,'flip_request_lateness_us'):12d} {i(d,'flip_call_us'):9d} {i(d,'flip_lateness_wait_us'):11d} {i(d,'toggle_lateness_wait_us'):11d}")
    if delays:
        print('\nLate-commit runtime attribution:')
        by={i(d,'boundary'):[] for d in delays}
        for t in tasks: by.setdefault(i(t,'boundary'),[]).append(t)
        for d in delays:
            b=i(d,'boundary')
            print(f"b{b}: entry={i(d,'wake_lateness_us')}us request={i(d,'request_lateness_us')}us flip_call={i(d,'flip_call_us')}us commit={i(d,'commit_lateness_us')}us runtime_window={i(d,'runtime_window_us')}us valid={i(d,'runtime_valid')}")
            for t in by.get(b,[])[:8]:
                print(f"    {t.get('task')} p{t.get('priority')} core={t.get('core')} +{t.get('runtime_delta_us')}us")
    if status:
        print('\nSTATUS request path:')
        print(' b lead  rxtrace  lockwait lockhold discipline wifi  sendto handler')
        for d in status:
            print(f"{i(d,'boundary'):2d} {i(d,'lead_us'):5d} {i(d,'rx_trace_end_us')-i(d,'rx_trace_begin_us'):7d} {i(d,'lock_acquired_us')-i(d,'lock_request_us'):8d} {i(d,'lock_release_us')-i(d,'lock_acquired_us'):8d} {i(d,'discipline_end_us')-i(d,'discipline_begin_us'):10d} {i(d,'wifi_end_us')-i(d,'wifi_begin_us'):5d} {i(d,'sendto_return_us')-i(d,'sendto_entry_us'):7d} {i(d,'handler_exit_us')-i(d,'recv_us'):7d}")
    if tx:
        print('\nAll firmware STATUS sends:')
        for d in tx:
            dur=i(d,'end_us')-i(d,'begin_us')
            print(f"reason={d.get('reason')} rem={d.get('remaining')} begin={d.get('begin_us')} sendto={i(d,'sendto_return_us')-i(d,'sendto_entry_us')}us total={dur}us")
    if args.controller_csv and args.controller_csv.exists():
        print('\nController sweep sends:')
        with args.controller_csv.open(newline='', encoding='utf-8-sig') as f:
            for r in csv.DictReader(f):
                if r.get('PacketType','').startswith('STATUS_SWEEP_'):
                    print(f"{r['PacketType']} {r['Destination']} send_lead={r['LeadToNextBoundaryUs']}us")

if __name__=='__main__': main()
