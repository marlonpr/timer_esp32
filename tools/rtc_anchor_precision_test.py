#!/usr/bin/env python3
"""Regression test for the per-SQW re-anchor quantization bug.

Models the exact fixed-rate case that exposed the old integer-microsecond anchor
loss. A 4.245 ppm local clock produces integer-microsecond SQW timestamps. The
legacy algorithm re-anchors after truncating disciplined time to whole us; the
new algorithm carries the anchor in ns and rounds only below 1 ns.
"""

RATE_PPB = 4245
SECONDS = 6000
NS_PER_S = 1_000_000_000
DEN = NS_PER_S + RATE_PPB


def c_round_div(n: int, d: int) -> int:
    # Inputs are positive in this test; equivalent to the firmware helper.
    return (n + d // 2) // d


def edge_local_us(second: int) -> int:
    return round(second * (1_000_000 + RATE_PPB / 1000.0))


def legacy_us() -> int:
    anchor_local = 0
    anchor_disciplined = 0
    for second in range(1, SECONDS + 1):
        local = edge_local_us(second)
        delta = local - anchor_local
        anchor_disciplined += (delta * NS_PER_S) // DEN
        anchor_local = local
    return anchor_disciplined


def fixed_ns() -> int:
    anchor_local = 0
    anchor_disciplined_ns = 0
    for second in range(1, SECONDS + 1):
        local = edge_local_us(second)
        delta_ns = (local - anchor_local) * 1000
        anchor_disciplined_ns += c_round_div(delta_ns * NS_PER_S, DEN)
        anchor_local = local
    return anchor_disciplined_ns


expected_us = SECONDS * 1_000_000
legacy_error_us = legacy_us() - expected_us
fixed_error_ns = fixed_ns() - expected_us * 1000

print(f"legacy_error_us={legacy_error_us}")
print(f"legacy_bias_ppm={legacy_error_us / SECONDS:.6f}")
print(f"fixed_error_ns={fixed_error_ns}")
print(f"fixed_bias_ppm={(fixed_error_ns / 1000) / SECONDS:.9f}")

assert legacy_error_us == -4530
assert abs(fixed_error_ns) <= 1
print("PASS")
