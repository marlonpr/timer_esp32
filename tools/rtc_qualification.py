#!/usr/bin/env python3
"""Analyze Factory Timer DS3231 relative-rate qualification logs.

The firmware reports master-minus-local clock offsets at each synchronized START.
For two STARTs separated in time:

    q = E - M = -d(master_minus_local_offset)/dt
    r = E - D  (firmware RTC discipline rate_ppm)
    D - M = q - r

The PC/master rate M cancels when comparing devices, so differences of D-M are
DS3231-to-DS3231 rate differences. This tool deliberately does not interpret
D-M as absolute error versus true time.
"""

from __future__ import annotations

import argparse
import math
import re
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


def parse_fields(line: str, marker: str) -> dict[str, str] | None:
    line = ANSI_RE.sub("", line)
    pos = line.find(marker)
    if pos < 0:
        return None
    fields: dict[str, str] = {}
    for token in line[pos + len(marker):].strip().split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value.rstrip(",")
    return fields


@dataclass
class QualSample:
    local_us: int
    rate_ppm: float
    rms_us: float
    temp_c: float | None
    state: str


@dataclass
class StartRecord:
    command: str
    sync: str
    sync_local_us: int
    sync_estimated_master_us: int
    offset_master_minus_local_us: int
    best_rtt_us: int
    target_master_us: int
    actual_local_us: int
    estimated_master_us: int
    start_error_us: int
    scheduler_lateness_us: int
    sync_age_local_us: int


@dataclass
class DeviceLog:
    path: Path
    device: str
    samples: list[QualSample]
    starts: list[StartRecord]


@dataclass
class Result:
    device: str
    path: Path
    interval_s: float
    delta_offset_us: int
    q_ppm: float
    r_mean_ppm: float
    r_sd_ppm: float
    d_minus_master_ppm: float
    samples: int
    mean_rms_us: float
    mean_temp_c: float | None
    start_a: StartRecord
    start_b: StartRecord


def require(fields: dict[str, str], names: Iterable[str], line_kind: str) -> None:
    missing = [name for name in names if name not in fields]
    if missing:
        raise ValueError(f"{line_kind} line missing fields: {', '.join(missing)}")


def load_log(path: Path) -> DeviceLog:
    samples: list[QualSample] = []
    starts: list[StartRecord] = []
    devices: set[str] = set()

    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        fields = parse_fields(raw, "RTC_QUAL ")
        if fields is not None:
            require(fields, ["device", "local_us", "state", "rate_ppm", "rms_us"], "RTC_QUAL")
            devices.add(fields["device"])
            temp: float | None = None
            if fields.get("temp_valid") == "1" and "temp_c" in fields:
                temp = float(fields["temp_c"])
            samples.append(
                QualSample(
                    local_us=int(fields["local_us"]),
                    rate_ppm=float(fields["rate_ppm"]),
                    rms_us=float(fields["rms_us"]),
                    temp_c=temp,
                    state=fields["state"],
                )
            )
            continue

        fields = parse_fields(raw, "RTC_START_QUAL ")
        if fields is not None:
            require(
                fields,
                [
                    "device", "command", "sync", "sync_local_us",
                    "sync_estimated_master_us", "offset_master_minus_local_us",
                    "best_rtt_us", "target_master_us", "actual_local_us",
                    "estimated_master_us", "start_error_us",
                    "scheduler_lateness_us", "sync_age_local_us",
                ],
                "RTC_START_QUAL",
            )
            devices.add(fields["device"])
            starts.append(
                StartRecord(
                    command=fields["command"],
                    sync=fields["sync"],
                    sync_local_us=int(fields["sync_local_us"]),
                    sync_estimated_master_us=int(fields["sync_estimated_master_us"]),
                    offset_master_minus_local_us=int(fields["offset_master_minus_local_us"]),
                    best_rtt_us=int(fields["best_rtt_us"]),
                    target_master_us=int(fields["target_master_us"]),
                    actual_local_us=int(fields["actual_local_us"]),
                    estimated_master_us=int(fields["estimated_master_us"]),
                    start_error_us=int(fields["start_error_us"]),
                    scheduler_lateness_us=int(fields["scheduler_lateness_us"]),
                    sync_age_local_us=int(fields["sync_age_local_us"]),
                )
            )

    if not devices:
        raise ValueError(f"{path}: no RTC_QUAL or RTC_START_QUAL records found")
    if len(devices) != 1:
        raise ValueError(f"{path}: contains multiple device IDs: {sorted(devices)}")
    return DeviceLog(path=path, device=next(iter(devices)), samples=samples, starts=starts)


def resolve_index(index: int, length: int) -> int:
    resolved = index if index >= 0 else length + index
    if resolved < 0 or resolved >= length:
        raise IndexError(f"START index {index} is out of range for {length} START records")
    return resolved


def analyze(log: DeviceLog, start_a_index: int, start_b_index: int) -> Result:
    if len(log.starts) < 2:
        raise ValueError(f"{log.path}: need at least two RTC_START_QUAL records")

    ia = resolve_index(start_a_index, len(log.starts))
    ib = resolve_index(start_b_index, len(log.starts))
    if ia == ib:
        raise ValueError(f"{log.path}: START A and START B must be different")

    a = log.starts[ia]
    b = log.starts[ib]
    if b.target_master_us <= a.target_master_us:
        raise ValueError(f"{log.path}: START B must have a later master target than START A")

    # The offset was measured/applied at SYNC_SET, not at the later START. Use
    # the copied sync epochs for both delta-time and the r=E-D averaging window
    # so different sync-to-START ages do not bias q.
    delta_master_us = b.sync_estimated_master_us - a.sync_estimated_master_us
    if delta_master_us <= 0:
        raise ValueError(f"{log.path}: START B must contain a later SYNC_SET epoch than START A")
    delta_offset_us = b.offset_master_minus_local_us - a.offset_master_minus_local_us
    q_ppm = -(delta_offset_us / delta_master_us) * 1_000_000.0

    # Average r=E-D over the exact same sync-to-sync interval.
    lo = min(a.sync_local_us, b.sync_local_us)
    hi = max(a.sync_local_us, b.sync_local_us)
    interval_samples = [
        sample for sample in log.samples
        if lo <= sample.local_us <= hi and sample.state == "LOCKED"
    ]
    if not interval_samples:
        raise ValueError(
            f"{log.path}: no LOCKED RTC_QUAL samples between the selected SYNC_SET epochs"
        )

    rates = [sample.rate_ppm for sample in interval_samples]
    r_mean = statistics.fmean(rates)
    r_sd = statistics.pstdev(rates) if len(rates) > 1 else 0.0
    mean_rms = statistics.fmean(sample.rms_us for sample in interval_samples)
    temperatures = [sample.temp_c for sample in interval_samples if sample.temp_c is not None]
    mean_temp = statistics.fmean(temperatures) if temperatures else None

    return Result(
        device=log.device,
        path=log.path,
        interval_s=delta_master_us / 1_000_000.0,
        delta_offset_us=delta_offset_us,
        q_ppm=q_ppm,
        r_mean_ppm=r_mean,
        r_sd_ppm=r_sd,
        d_minus_master_ppm=q_ppm - r_mean,
        samples=len(interval_samples),
        mean_rms_us=mean_rms,
        mean_temp_c=mean_temp,
        start_a=a,
        start_b=b,
    )


def fmt_temp(value: float | None) -> str:
    return "n/a" if value is None else f"{value:.2f}"


def print_results(results: list[Result], duration_s: float) -> None:
    print("\nPer-device relative-rate result")
    print("device  interval_s  dOffset_us   q=E-M ppm   mean r=E-D ppm   r_sd ppm   D-M ppm   N   rms_us   temp_C  ageD_ms")
    for r in results:
        print(
            f"{r.device:6s} {r.interval_s:10.3f} {r.delta_offset_us:11d} "
            f"{r.q_ppm:+11.6f} {r.r_mean_ppm:+16.6f} {r.r_sd_ppm:10.6f} "
            f"{r.d_minus_master_ppm:+9.6f} {r.samples:3d} {r.mean_rms_us:8.3f} {fmt_temp(r.mean_temp_c):>8s} "
            f"{(r.start_b.sync_age_local_us - r.start_a.sync_age_local_us) / 1000.0:+8.3f}"
        )

    median_d = statistics.median(r.d_minus_master_ppm for r in results)
    print(f"\nBatch median D-M: {median_d:+.6f} ppm")
    print(f"Predicted deviation from batch median after {duration_s:.3f} s:")
    for r in results:
        deviation = r.d_minus_master_ppm - median_d
        drift_ms = deviation * duration_s / 1000.0  # ppm*s = microseconds
        print(
            f"  {r.device}: deviation={deviation:+.6f} ppm, "
            f"relative_time={drift_ms:+.3f} ms"
        )

    if len(results) == 2:
        delta = results[0].d_minus_master_ppm - results[1].d_minus_master_ppm
        drift_ms = delta * duration_s / 1000.0
        print(
            f"\nPair {results[0].device}-{results[1].device}: "
            f"D1-D2={delta:+.6f} ppm, predicted separation={drift_ms:+.3f} ms "
            f"after {duration_s:.3f} s"
        )
    elif len(results) > 2:
        d_values = [r.d_minus_master_ppm for r in results]
        spread = max(d_values) - min(d_values)
        print(
            f"\nBatch max-min RTC rate spread: {spread:.6f} ppm, "
            f"equivalent to {spread * duration_s / 1000.0:.3f} ms over {duration_s:.3f} s"
        )

    print("\nInterpretation: D-M is relative to the PC master, not absolute true-time error.")
    print("Only differences between devices (or deviation from the batch median) qualify RTC agreement.")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze two-START DS3231 qualification logs without treating the PC as an absolute reference."
    )
    parser.add_argument("logs", nargs="+", type=Path, help="one serial-monitor log file per device")
    parser.add_argument("--start-a", type=int, default=0, help="first RTC_START_QUAL index (default: 0)")
    parser.add_argument("--start-b", type=int, default=-1, help="second RTC_START_QUAL index (default: -1, last)")
    parser.add_argument(
        "--duration",
        type=float,
        default=5999.0,
        help="countdown duration in seconds for predicted separation (default: 5999 = 99:59)",
    )
    args = parser.parse_args()

    if args.duration <= 0 or not math.isfinite(args.duration):
        parser.error("--duration must be a finite positive number")

    try:
        results = [
            analyze(load_log(path), args.start_a, args.start_b)
            for path in args.logs
        ]
    except (OSError, ValueError, IndexError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print_results(results, args.duration)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
