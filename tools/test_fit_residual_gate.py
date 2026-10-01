#!/usr/bin/env python3
"""Arithmetic smoke test for the v6.19 fit-only SQW timestamp outlier gate."""

THRESHOLD_US = 4.0

def center_outlier(a, b, c, expected):
    d1 = (b-a)-expected
    d2 = (c-b)-expected
    if abs(d1) < THRESHOLD_US or abs(d2) < THRESHOLD_US or d1*d2 >= 0:
        return None
    return 0.5*(d1-d2)

def run():
    p=1_000_004.5
    # clean quantized intervals
    assert center_outlier(0, 1_000_004, 2_000_009, p) is None
    # +4.5 us displaced center -> +4.5/-4.5 interval residual pair
    r=center_outlier(0, 1_000_009, 2_000_009, p)
    assert r is not None and 4.0 <= r <= 5.0, r
    # +33.5 us displaced center -> 67 us p2p signature
    r=center_outlier(0, 1_000_038, 2_000_009, p)
    assert r is not None and 33.0 <= r <= 34.0, r
    # Current endpoint delayed: previous center must not be rejected yet.
    assert center_outlier(0, 1_000_004, 2_000_043, p) is None
    # Smooth common rate error has same-sign residuals and is not an outlier.
    assert center_outlier(0, 1_000_010, 2_000_020, p) is None
    print('v6.19 residual-gate arithmetic: PASS')

if __name__ == '__main__':
    run()
