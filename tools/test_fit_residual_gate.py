#!/usr/bin/env python3
"""Arithmetic smoke test for the v6.23 one-sided SQW timestamp gate."""

THRESHOLD_US = 4.0

def late_center_outlier(a, b, c, expected):
    d1 = (b-a)-expected
    d2 = (c-b)-expected
    # ISR latency can only move the shared center timestamp later:
    # +d on the first interval and -d on the second.
    if d1 < THRESHOLD_US or d2 > -THRESHOLD_US:
        return None
    return 0.5*(d1-d2)

def run():
    p=1_000_004.5
    # Clean quantized intervals.
    assert late_center_outlier(0, 1_000_004, 2_000_009, p) is None

    # +4.5 us delayed center -> +4.5/-4.5 residual pair: reject.
    r=late_center_outlier(0, 1_000_009, 2_000_009, p)
    assert r is not None and 4.0 <= r <= 5.0, r

    # +33.5 us delayed center -> 67 us p2p signature: reject.
    r=late_center_outlier(0, 1_000_038, 2_000_009, p)
    assert r is not None and 33.0 <= r <= 34.0, r

    # -8.5 us apparently early center -> -8.5/+8.5: KEEP.
    # This is the key v6.23 change versus the symmetric v6.19-v6.22 gate.
    assert late_center_outlier(0, 999_996, 2_000_009, p) is None

    # Current endpoint delayed: previous center must not be rejected yet.
    assert late_center_outlier(0, 1_000_004, 2_000_043, p) is None

    # Previous endpoint delayed: center has only one directional residual.
    assert late_center_outlier(20, 1_000_004, 2_000_009, p) is None

    # Smooth common rate error has same-sign residuals and is not an outlier.
    assert late_center_outlier(0, 1_000_010, 2_000_020, p) is None

    print('v6.23 late-only residual-gate arithmetic: PASS')

if __name__ == '__main__':
    run()
