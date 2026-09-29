#!/usr/bin/env python3
import argparse
import math
import re
import statistics
from pathlib import Path

FW_SQW_RE = re.compile(r"SQW_TRACE sequence=(\d+) local_us=(\d+) core=(-?\d+)")
FW_COMMIT_RE = re.compile(
    r"ISR_PUBLISH boundary=(\d+).*?publish_marker_end_us=(\d+).*?marker_level=(\d+)"
)
ANZ_RE = re.compile(
    r"ANZ\|EDGE\|\d+\|\d+\|\d+\|(ESP01_SQW|ESP01_COMMIT)\|(\d+)\|(\d+)"
)
STATUS_TX_RE = re.compile(
    r"STATUS_TX reason=([^ ]+).*?wifi_begin_us=(\d+) wifi_end_us=(\d+).*?"
    r"sendto_entry_us=(\d+) sendto_return_us=(\d+)"
)
STATUS_PATH_RE = re.compile(
    r"STATUS_PATH .*?ip_input_us=(-?\d+).*?recv_us=(\d+)"
)
SYNC_TX_RE = re.compile(
    r"SYNC_REPLY_TRACE .*?sendto_entry_us=(\d+) sendto_return_us=(\d+)"
)


def linfit(xs, ys):
    mx = sum(xs) / len(xs)
    my = sum(ys) / len(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        raise ValueError("zero x variance")
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    a = my - b * mx
    residuals = [y - (a + b * x) for x, y in zip(xs, ys)]
    return a, b, residuals


def percentile(values, p):
    if not values:
        return float("nan")
    ordered = sorted(values)
    pos = (len(ordered) - 1) * p
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] * (hi - pos) + ordered[hi] * (pos - lo)


def parse_firmware(path):
    sqw = []
    commits = []
    net_intervals = []
    ingress_intervals = []
    for line in Path(path).read_text(errors="replace").splitlines():
        m = FW_SQW_RE.search(line)
        if m:
            sqw.append({"seq": int(m[1]), "t": int(m[2]), "core": int(m[3])})
            continue
        m = FW_COMMIT_RE.search(line)
        if m:
            commits.append({"boundary": int(m[1]), "t": int(m[2]), "level": int(m[3])})
            continue
        m = STATUS_TX_RE.search(line)
        if m:
            reason = m[1]
            wb, we, sb, se = map(int, m.groups()[1:])
            net_intervals.append((wb, we, f"wifi:{reason}"))
            net_intervals.append((sb, se, f"sendto:{reason}"))
            continue
        m = STATUS_PATH_RE.search(line)
        if m:
            ip_in, recv = map(int, m.groups())
            if ip_in >= 0 and recv >= ip_in:
                ingress_intervals.append((ip_in, recv, "ip_input_to_recv"))
            continue
        m = SYNC_TX_RE.search(line)
        if m:
            sb, se = map(int, m.groups())
            if se >= sb:
                net_intervals.append((sb, se, "sendto:SYNC_REPLY"))
    return sqw, commits, net_intervals, ingress_intervals


def parse_analyzer(path):
    sqw_rising = []
    commits = []
    for line in Path(path).read_text(errors="replace").splitlines():
        m = ANZ_RE.search(line)
        if not m:
            continue
        name, t, level = m[1], int(m[2]), int(m[3])
        if name == "ESP01_SQW":
            if level == 1:
                sqw_rising.append(t)
        elif name == "ESP01_COMMIT":
            commits.append({"t": t, "level": level})
    return sqw_rising, commits


def fit_commit_anchor(fw_commits, anz_commits):
    if len(fw_commits) < 6 or len(anz_commits) < 6:
        raise SystemExit(
            f"need >=6 GPIO33 commits in both logs; firmware={len(fw_commits)} analyzer={len(anz_commits)}"
        )
    best = None
    # Try every contiguous overlap. GPIO33 level is used as a hard parity check.
    for fw_start in range(len(fw_commits)):
        for anz_start in range(len(anz_commits)):
            n = min(len(fw_commits) - fw_start, len(anz_commits) - anz_start)
            if n < 6:
                continue
            pairs = []
            for k in range(n):
                f = fw_commits[fw_start + k]
                a = anz_commits[anz_start + k]
                if f["level"] != a["level"]:
                    break
                pairs.append((f, a))
            if len(pairs) < 6:
                continue
            xs = [p[0]["t"] for p in pairs]
            ys = [p[1]["t"] for p in pairs]
            intercept, slope, residuals = linfit(xs, ys)
            if not (0.995 <= slope <= 1.005):
                continue
            rms = math.sqrt(sum(r * r for r in residuals) / len(residuals))
            # Prefer longest level-consistent run, then smallest residual.
            score = (-len(pairs), rms)
            if best is None or score < best[0]:
                best = (score, pairs, intercept, slope, residuals)
    if best is None:
        raise SystemExit("could not align firmware/analyzer GPIO33 commit sequences")
    _, pairs, intercept, slope, residuals = best
    return pairs, intercept, slope, residuals


def classify_intervals(t_fw, intervals, margin_us=0):
    labels = []
    for start, end, label in intervals:
        if start - margin_us <= t_fw <= end + margin_us:
            labels.append(label)
    return labels


def main():
    ap = argparse.ArgumentParser(
        description=(
            "Analyze v6.17 raw SQW timestamp latency. GPIO33 commits first anchor "
            "the analyzer clock to the ESP32 clock; physical SQW edges are then "
            "matched without ambiguous integer-second alignment."
        )
    )
    ap.add_argument("firmware_log")
    ap.add_argument("analyzer_log")
    ap.add_argument("--threshold-us", type=float, default=3.0,
                    help="flag |latency - median| above this value (default: 3 us)")
    ap.add_argument("--network-margin-us", type=int, default=0,
                    help="optional margin around logged application network intervals")
    args = ap.parse_args()

    fw_sqw, fw_commits, net_intervals, ingress_intervals = parse_firmware(args.firmware_log)
    anz_sqw, anz_commits = parse_analyzer(args.analyzer_log)
    if len(fw_sqw) < 8 or len(anz_sqw) < 8:
        raise SystemExit(f"need >=8 SQW rising edges; firmware={len(fw_sqw)} analyzer={len(anz_sqw)}")

    physical_periods = [b - a for a, b in zip(anz_sqw, anz_sqw[1:])]
    if physical_periods:
        pmean = sum(physical_periods) / len(physical_periods)
        prms = math.sqrt(sum((x - pmean) ** 2 for x in physical_periods) / len(physical_periods))
        print(
            f"analyzer_sqw_period_mean_us={pmean:.3f} rms_us={prms:.3f} "
            f"p2p_us={max(physical_periods)-min(physical_periods):.3f}"
        )

    commit_pairs, intercept, slope, commit_residuals = fit_commit_anchor(fw_commits, anz_commits)
    commit_rms = math.sqrt(sum(r * r for r in commit_residuals) / len(commit_residuals))
    print(
        f"commit_anchor_pairs={len(commit_pairs)} analyzer_per_firmware={slope:.12f} "
        f"commit_fit_rms_us={commit_rms:.3f}"
    )

    # Transform each analyzer SQW rising edge into the firmware-local timebase.
    physical_fw = [(t - intercept) / slope for t in anz_sqw]
    matched = []
    used = set()
    for f in fw_sqw:
        # nearest physical rising edge in the anchored timebase
        best_j = None
        best_abs = None
        for j, t_phys in enumerate(physical_fw):
            if j in used:
                continue
            d = abs(t_phys - f["t"])
            if best_abs is None or d < best_abs:
                best_abs = d
                best_j = j
        if best_j is None or best_abs is None or best_abs > 250_000:
            continue
        used.add(best_j)
        t_phys = physical_fw[best_j]
        latency = f["t"] - t_phys
        matched.append((f, anz_sqw[best_j], t_phys, latency))

    if len(matched) < 8:
        raise SystemExit(f"only {len(matched)} anchored SQW pairs; expected >=8")

    latencies = [x[3] for x in matched]
    median = statistics.median(latencies)
    centered = [x - median for x in latencies]
    rms = math.sqrt(sum(x * x for x in centered) / len(centered))
    abs_centered = [abs(x) for x in centered]
    print(
        f"sqw_pairs={len(matched)} latency_median_us={median:.3f} "
        f"centered_rms_us={rms:.3f} p95_abs_us={percentile(abs_centered, 0.95):.3f} "
        f"min_centered_us={min(centered):.3f} max_centered_us={max(centered):.3f}"
    )

    # 1-us histogram of anchored latency. Useful for seeing discrete modes.
    bins = {}
    for value in latencies:
        key = int(round(value))
        bins[key] = bins.get(key, 0) + 1
    print("latency_histogram_rounded_us=" + " ".join(f"{k}:{bins[k]}" for k in sorted(bins)))

    flags = []
    for item, delta in zip(matched, centered):
        if abs(delta) > args.threshold_us:
            flags.append((item, delta))
    print(f"excursions_abs_from_median_gt_{args.threshold_us:g}us={len(flags)}")
    for (f, analyzer_t, physical_t, latency), delta in flags:
        labels = classify_intervals(f["t"], net_intervals, args.network_margin_us)
        ingress = classify_intervals(f["t"], ingress_intervals, args.network_margin_us)
        all_labels = labels + ingress
        label_text = ",".join(all_labels) if all_labels else "none"
        print(
            f"seq={f['seq']} fw_local_us={f['t']} analyzer_sqw_us={analyzer_t} "
            f"latency_us={latency:.3f} delta_from_median_us={delta:+.3f} "
            f"core={f['core']} overlap={label_text}"
        )


if __name__ == "__main__":
    main()
