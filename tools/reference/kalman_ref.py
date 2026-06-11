#!/usr/bin/env python3
"""Independent NumPy reference for the constant-velocity Kalman filter.

Mirrors sim/tracking.cpp's KalmanFilter (F, DWNA Q, H, Joseph-form update)
for a fixed, fully deterministic measurement sequence and writes sampled
filter states and covariances to tests/golden/kalman_ref.csv. The C++ unit
test runs the same sequence and compares — a cross-language check of the
predict/update algebra (charter §5.5 검증, same pattern as ballistics_ref.py).

The measurement "noise" is a closed-form pseudo-noise (sums of sines), NOT a
PRNG: random number generators differ between languages, and the subject
under test is the filter algebra, not the noise source.

Usage: python3 tools/reference/kalman_ref.py
"""

import csv
import math
import os

import numpy as np

DT = 1.0 / 60.0
ACCEL_NOISE = 30.0          # sigma_a, m/s^2 (DWNA)
STEPS = 60
SAMPLED_STEPS = (1, 2, 5, 20, 60)

# Truth: constant velocity.
TRUTH_X0 = np.array([8000.0, 6000.0, 300.0, -180.0, -120.0, 0.0])

# Filter initialization (deliberately offset from truth).
X0 = np.array([8050.0, 5970.0, 320.0, 0.0, 0.0, 0.0])
P0 = np.diag([900.0, 900.0, 400.0, 160000.0, 160000.0, 160000.0])

# Fixed diagonal measurement covariance.
R = np.diag([100.0, 64.0, 81.0])


def pseudo_noise(k: int) -> np.ndarray:
    """Deterministic, language-portable stand-in for measurement noise."""
    return np.array([
        9.0 * math.sin(0.7 * k + 0.3),
        7.0 * math.sin(1.1 * k + 1.2),
        5.0 * math.sin(1.9 * k + 2.1),
    ])


def main() -> None:
    f = np.eye(6)
    for axis in range(3):
        f[axis, axis + 3] = DT
    sigma_sq = ACCEL_NOISE * ACCEL_NOISE
    q = np.zeros((6, 6))
    for axis in range(3):
        q[axis, axis] = sigma_sq * DT**4 / 4.0
        q[axis, axis + 3] = sigma_sq * DT**3 / 2.0
        q[axis + 3, axis] = q[axis, axis + 3]
        q[axis + 3, axis + 3] = sigma_sq * DT**2
    h = np.zeros((3, 6))
    h[0, 0] = h[1, 1] = h[2, 2] = 1.0

    x = X0.copy()
    p = P0.copy()
    truth = TRUTH_X0.copy()

    rows = []
    for step in range(1, STEPS + 1):
        truth = f @ truth
        x = f @ x
        p = f @ p @ f.T + q

        z = truth[:3] + pseudo_noise(step)
        s = h @ p @ h.T + R
        k = p @ h.T @ np.linalg.inv(s)
        x = x + k @ (z - h @ x)
        i_kh = np.eye(6) - k @ h
        p = i_kh @ p @ i_kh.T + k @ R @ k.T
        p = (p + p.T) / 2.0

        if step in SAMPLED_STEPS:
            row = [step] + [repr(float(v)) for v in x]
            for r_idx in range(6):
                for c_idx in range(r_idx + 1):
                    row.append(repr(float(p[r_idx, c_idx])))
            rows.append(row)

    out_path = os.path.join(os.path.dirname(__file__), "..", "..", "tests", "golden",
                            "kalman_ref.csv")
    header = ["step"] + [f"x{i}" for i in range(6)]
    header += [f"p{r}{c}" for r in range(6) for c in range(r + 1)]
    with open(out_path, "w", newline="") as out:
        writer = csv.writer(out, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(rows)
    print(f"wrote {len(rows)} samples to {os.path.normpath(out_path)}")


if __name__ == "__main__":
    main()
