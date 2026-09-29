#!/usr/bin/env python3
"""Analyze v6.12 BOUNDARY_TRACE lines from one ESP32 serial log.

The trace is buffered during the run and dumped afterward. This script focuses on
whether the disciplined->local inverse mapping itself moved by milliseconds.
"""
from __future__ import annotations

import argparse
import re
import statistics
from pathlib import Path

TRACE_RE = re.compile(
    r"BOUNDARY_TRACE boundary=(?P<boundary>\d+) "
    r"disciplined_us=(?P<disc>-?\d+) "
    r"boundary_local_us=(?P<local>-?\d+) "
    r"step_us=(?P<step>-?\d+) "
    r"flip_commit_us=(?P<flip>-?\d+) "
    r"toggle_after_us=(?P<toggle>-?\d+) "
    r"post_local_us=(?P<post>-?\d+) "
    r"post_minus_wait_us=(?P<postdiff>-?\d+) "
    r"flip_lateness_wait_us=(?P<latewait>-?\d+) "
    r"toggle_lateness_wait_us=(?P<togglelate>-?\d+) "
    r"flip_lateness_post_us=(?P<latepost>-?\d+)"
)


def median(values):
    return statistics.median(values) if values else float('nan')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", type=Path)
    ap.add_argument("--jump-us", type=float, default=100.0,
                    help="flag deadline-step deviations larger than this (default 100 us)")
    args = ap.parse_args()

    rows = []
    for line in args.log.read_text(errors="replace").splitlines():
        m = TRACE_RE.search(line)
        if not m:
            continue
        d = {k: int(v) for k, v in m.groupdict().items()}
        rows.append(d)

    if not rows:
        print("No BOUNDARY_TRACE rows found.")
        return 2

    normal_steps = [r["step"] for r in rows if r["boundary"] > 1 and r["step"] != 0]
    med_step = median(normal_steps)
    print(f"rows={len(rows)}")
    print(f"median_deadline_step_us={med_step:.3f}")
    print(f"expected_rate_ppm_from_step={(med_step - 1_000_000.0):+.6f}")
    print()
    print("boundary  local_deadline       step   step_dev   flip_late  toggle_late  post-wait")

    flagged = 0
    for r in rows:
        step_dev = 0.0 if r["step"] == 0 else r["step"] - med_step
        flag = (r["step"] != 0 and abs(step_dev) > args.jump_us) or abs(r["postdiff"]) > args.jump_us
        if flag:
            flagged += 1
        mark = "  <--" if flag else ""
        print(
            f"{r['boundary']:8d}  {r['local']:14d}  {r['step']:9d}  "
            f"{step_dev:+9.1f}  {r['latewait']:+9d}  {r['togglelate']:+11d}  "
            f"{r['postdiff']:+9d}{mark}"
        )

    print()
    print(f"mapping_flags={flagged}")
    print(f"max_abs_post_minus_wait_us={max(abs(r['postdiff']) for r in rows)}")
    print(f"max_flip_lateness_against_wait_deadline_us={max(r['latewait'] for r in rows)}")
    print(f"max_toggle_lateness_against_wait_deadline_us={max(r['togglelate'] for r in rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
