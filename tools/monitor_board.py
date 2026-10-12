#!/usr/bin/env python3
"""Capture plain serial output from a verified board build, without flash/reset commands."""
from pathlib import Path
import argparse
import json
import math
import re
import sys
import time


class BoardIdentityError(RuntimeError):
    pass


class BoardIdentityGuard:
    """Check complete RTC_QUAL lines across arbitrary serial read boundaries."""
    device_pattern = re.compile(rb"\bRTC_QUAL\b[^\r\n]*?\bdevice=(ESP[0-9]{2})\b")

    def __init__(self, board: str, started: float, timeout_seconds: float, accepted_before_open: int | None = None):
        self.board = board
        self.started = started
        self.timeout_seconds = timeout_seconds
        self.verified = False
        self.accepted_before_open = accepted_before_open
        self.first_accepted_edges = None
        self.last_accepted_edges = None
        self.pending_line = bytearray()

    def observe(self, data: bytes, now: float) -> None:
        if not self.verified and now - self.started >= self.timeout_seconds:
            raise BoardIdentityError(
                f"PORT_IDENTITY_UNVERIFIED expected={self.board}: no RTC_QUAL identity "
                f"within {self.timeout_seconds:g} s; no verified capture written")
        self.pending_line.extend(data)
        while (end := self.pending_line.find(b"\n")) >= 0:
            line = bytes(self.pending_line[:end]).rstrip(b"\r")
            del self.pending_line[:end + 1]
            match = self.device_pattern.search(line)
            if match is None:
                continue
            observed = match.group(1).decode("ascii")
            if observed != self.board:
                raise BoardIdentityError(
                    f"PORT_IDENTITY_MISMATCH expected={self.board} observed={observed}; capture stopped")
            accepted = re.search(rb"\baccepted=(\d+)\b", line)
            if accepted is None and self.accepted_before_open is not None:
                raise BoardIdentityError("PORT_OPEN_CONTINUITY_UNVERIFIED: RTC_QUAL accepted count missing")
            if accepted is not None:
                count = int(accepted.group(1))
                if self.first_accepted_edges is None:
                    if self.accepted_before_open is not None and count < self.accepted_before_open:
                        raise BoardIdentityError(
                            f"PORT_OPEN_RESET_DETECTED board={self.board} accepted_before_open={self.accepted_before_open} accepted_after_open={count}; restart warm-up")
                    self.first_accepted_edges = count
                if self.last_accepted_edges is not None and count < self.last_accepted_edges:
                    raise BoardIdentityError(
                        f"RTC_ACCEPTED_EDGES_DECREASED board={self.board} previous={self.last_accepted_edges} current={count}; capture stopped")
                self.last_accepted_edges = count
            self.verified = True
        # Bound memory for a damaged stream without line endings.
        if len(self.pending_line) > 8192:
            raise BoardIdentityError("PORT_IDENTITY_CAPTURE_DAMAGED: serial line exceeds 8192 bytes")


def capture_verified(uart, board, write, *, identity_timeout_seconds=35.0,
                     clock=time.monotonic, on_verified=lambda: None, accepted_before_open=None, on_continuity=lambda guard: None):
    guard = BoardIdentityGuard(board, clock(), identity_timeout_seconds, accepted_before_open)
    pending = bytearray()
    announced = False
    while True:
        data = uart.read(max(1, uart.in_waiting))
        guard.observe(data, clock())
        if not data:
            continue
        if not guard.verified:
            pending.extend(data)
            if len(pending) > 262144:
                raise BoardIdentityError("PORT_IDENTITY_UNVERIFIED: capture buffer exceeded 256 KiB")
            continue
        if not announced:
            on_verified()
            on_continuity(guard)
            announced = True
        if pending:
            write(bytes(pending) + data)
            pending.clear()
        else:
            write(data)


def verify_build(build: Path, board: str) -> Path:
    build = build.resolve()
    header = (build / "config/sdkconfig.h").read_text(encoding="utf-8")
    match = re.search(r'^#define CONFIG_FACTORY_DEVICE_ID "(ESP\d{2})"$', header, re.M)
    if match is None or match.group(1) != board:
        raise ValueError(f"{board}: build identity is {match.group(1) if match else 'unknown'}: {build}")
    description = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    elf = (build / description["app_elf"]).resolve()
    if elf.parent != build or not elf.is_file():
        raise ValueError(f"ELF is absent or outside the verified build: {elf}")
    return elf


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", required=True, choices=[f"ESP{n:02}" for n in range(1, 16)])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--identity-timeout-seconds", type=float, default=35.0,
                        help="Wait for the requested board's RTC_QUAL identity before writing (default: 35 s).")
    parser.add_argument("--accepted-before-open", type=int,
                        help="Accepted-edge count from this board's fresh controller STATUS before opening; abort if the first RTC_QUAL count is lower.")
    args = parser.parse_args()
    try:
        elf = verify_build(args.build, args.board)
    except (OSError, ValueError, KeyError) as exc:
        parser.error(str(exc))
    print(f"MONITOR_ONLY board={args.board} elf={elf} port={args.port or '--'} "
          "port_identity=UNVERIFIED", file=sys.stderr)
    if args.check_only:
        return 0
    if not args.port:
        parser.error("--port is required for capture")
    if args.baud <= 0:
        parser.error("--baud must be positive")
    if not math.isfinite(args.identity_timeout_seconds) or args.identity_timeout_seconds <= 0:
        parser.error("--identity-timeout-seconds must be finite and positive")
    if args.accepted_before_open is not None and args.accepted_before_open < 0:
        parser.error("--accepted-before-open must be >= 0")
    if args.log and args.log.exists():
        parser.error(f"capture already exists: {args.log}")
    # pyserial is provided by the activated ESP-IDF Python environment.
    import serial
    uart = serial.Serial()  # Unopened until all modem-control settings are applied.
    uart.port = args.port
    uart.baudrate = args.baud
    uart.timeout = 0.2
    uart.rtscts = False
    uart.dsrdtr = False
    uart.dtr = False
    uart.rts = False
    log = None

    def write(data):
        nonlocal log
        # Do not create a board-named log until the live port identity matches.
        if args.log and log is None:
            log = args.log.open("xb", buffering=0)
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        if log is not None:
            log.write(data)

    try:
        uart.open()
        capture_verified(uart, args.board, write, identity_timeout_seconds=args.identity_timeout_seconds,
                         on_verified=lambda: print(
                             f"PORT_IDENTITY_VERIFIED board={args.board} port={args.port}", file=sys.stderr),
                         accepted_before_open=args.accepted_before_open,
                         on_continuity=lambda guard: print(
                             f"PORT_OPEN_CONTINUITY={'VERIFIED' if args.accepted_before_open is not None else 'UNVERIFIED'} "
                             f"board={args.board} accepted_before_open={args.accepted_before_open} "
                             f"accepted_after_open={guard.first_accepted_edges}", file=sys.stderr))
    except BoardIdentityError as exc:
        print(f"{exc} port={args.port}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 0
    finally:
        uart.close()
        if log is not None:
            log.close()


if __name__ == "__main__":
    raise SystemExit(main())
