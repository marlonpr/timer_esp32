#!/usr/bin/env python3
import argparse
import re
from pathlib import Path

GAP = re.compile(
    r"MARKER_GAP boundary=(?P<boundary>\d+).*?"
    r"flip_to_before_us=(?P<pre>-?\d+) "
    r"toggle_call_us=(?P<call>-?\d+) "
    r"flip_to_after_us=(?P<total>-?\d+) "
    r"runtime_interval_us=(?P<interval>-?\d+)"
)
TASK = re.compile(
    r"MARKER_GAP_TASK boundary=(?P<boundary>\d+) rank=(?P<rank>\d+) "
    r"task=(?P<task>\S+) core=(?P<core>-?\d+) priority=(?P<priority>\d+) "
    r"runtime_delta_us=(?P<delta>\d+)"
)


def main():
    ap = argparse.ArgumentParser(description="Summarize v6.11 marker-gap diagnostics")
    ap.add_argument("log", type=Path)
    ap.add_argument("--top", type=int, default=8)
    args = ap.parse_args()

    events = {}
    for line in args.log.read_text(errors="replace").splitlines():
        m = GAP.search(line)
        if m:
            b = int(m.group("boundary"))
            events[b] = {
                "pre": int(m.group("pre")),
                "call": int(m.group("call")),
                "total": int(m.group("total")),
                "interval": int(m.group("interval")),
                "tasks": [],
            }
            continue
        m = TASK.search(line)
        if m:
            b = int(m.group("boundary"))
            events.setdefault(b, {"pre": None, "call": None, "total": None,
                                  "interval": None, "tasks": []})
            events[b]["tasks"].append({
                "rank": int(m.group("rank")),
                "task": m.group("task"),
                "core": int(m.group("core")),
                "priority": int(m.group("priority")),
                "delta": int(m.group("delta")),
            })

    if not events:
        print("No MARKER_GAP events found.")
        return

    for b in sorted(events):
        e = events[b]
        print(f"boundary {b}: flip->before={e['pre']} us, toggle={e['call']} us, "
              f"total={e['total']} us, runtime_interval={e['interval']} us")
        for t in sorted(e["tasks"], key=lambda x: x["rank"])[:args.top]:
            print(f"  #{t['rank']:>2} {t['task']:<18} core={t['core']:>2} "
                  f"prio={t['priority']:>2} runtime_delta={t['delta']} us")


if __name__ == "__main__":
    main()
