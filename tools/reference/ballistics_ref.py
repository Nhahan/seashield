#!/usr/bin/env python3
"""Independent reference implementation of the rocket exterior ballistics.

Mirrors sim/ballistics.cpp + sim/environment.cpp for the no-thrust, no-wind,
dry-air case and writes sampled trajectory points to
tests/golden/ballistics_ref.csv. The C++ unit test integrates the same setup
and compares against these values — a cross-language check of the
atmosphere + drag + RK4 chain (charter §5.6 검증).

Usage: python3 tools/reference/ballistics_ref.py
"""

import csv
import math
import os

G = 9.80665
RD = 287.058
T0_K = 15.0 + 273.15
P0 = 101325.0
LAPSE = 0.0065

MASS = 60.0
CDA = 0.012
DT = 1.0 / 60.0

LAUNCH_SPEED = 300.0
ELEVATION = math.radians(30.0)
AZIMUTH = math.radians(90.0)  # East.

SAMPLE_STEPS = (60, 120, 300)  # t = 1s, 2s, 5s.


def density(altitude):
    h = max(0.0, altitude)
    t = max(1.0, T0_K - LAPSE * h)
    p = P0 * (t / T0_K) ** (G / (RD * LAPSE))
    return p / (RD * t)  # Dry air (humidity = 0).


def accel(p, v):
    ax, ay, az = 0.0, 0.0, -G
    speed = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2)
    if speed > 0.0:
        k = 0.5 * density(p[2]) * CDA / MASS
        ax -= k * speed * v[0]
        ay -= k * speed * v[1]
        az -= k * speed * v[2]
    return (ax, ay, az)


def add(a, b, s=1.0):
    return (a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s)


def rk4_step(p, v):
    half = DT * 0.5
    k1p, k1v = v, accel(p, v)
    k2p, k2v = add(v, k1v, half), accel(add(p, v, half), add(v, k1v, half))
    k3p, k3v = add(v, k2v, half), accel(add(p, k2p, half), add(v, k2v, half))
    k4p, k4v = add(v, k3v, DT), accel(add(p, k3p, DT), add(v, k3v, DT))
    dp = tuple((k1p[i] + 2 * k2p[i] + 2 * k3p[i] + k4p[i]) * (DT / 6) for i in range(3))
    dv = tuple((k1v[i] + 2 * k2v[i] + 2 * k3v[i] + k4v[i]) * (DT / 6) for i in range(3))
    return add(p, dp), add(v, dv)


def main():
    direction = (
        math.sin(AZIMUTH) * math.cos(ELEVATION),
        math.cos(AZIMUTH) * math.cos(ELEVATION),
        math.sin(ELEVATION),
    )
    p = (0.0, 0.0, 0.0)
    v = tuple(LAUNCH_SPEED * d for d in direction)

    out_path = os.path.join(os.path.dirname(__file__), "..", "..", "tests", "golden",
                            "ballistics_ref.csv")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["step", "x", "y", "z"])
        for step in range(1, max(SAMPLE_STEPS) + 1):
            p, v = rk4_step(p, v)
            if step in SAMPLE_STEPS:
                writer.writerow([step, repr(p[0]), repr(p[1]), repr(p[2])])
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
