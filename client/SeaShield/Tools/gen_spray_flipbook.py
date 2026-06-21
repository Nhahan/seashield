#!/usr/bin/env python3
"""Procedurally generate the spray FLIPBOOK sheet (T_SpraySheet) — an 8x8 grid of 64 frames showing one
aerated water-droplet cluster forming bright+compact and then SPREADING + DISSIPATING with age. M_Spray
samples it as SubUV (per-instance age -> frame), giving real droplet-cluster shape variety instead of a
single round billboard. Keeps the project's all-procedural discipline: numpy-generated, no hand-art.
RGBA: white RGB (the lit material tints it), alpha = the droplet shape. Deterministic (seeded)."""
import os
import numpy as np
from PIL import Image

GRID = 8                      # 8x8 = 64 frames
CELL = 64                     # px per frame
SIZE = GRID * CELL            # 512
rng = np.random.default_rng(7)
yy, xx = np.mgrid[0:CELL, 0:CELL].astype(np.float32)


def blob(cx, cy, r):
    return np.exp(-(((xx - cx) ** 2 + (yy - cy) ** 2) / (2.0 * r * r)))


def fbm(shape, octaves=4):
    """Cheap value-noise fBm in [0,1] for tearing the cluster into lace."""
    h, w = shape
    out = np.zeros(shape, np.float32)
    amp = 1.0
    for o in range(octaves):
        step = max(1, 2 ** (octaves - o))
        coarse = rng.random((h // step + 2, w // step + 2)).astype(np.float32)
        yi = (np.arange(h) / step).astype(int)
        xi = (np.arange(w) / step).astype(int)
        out += amp * coarse[np.ix_(yi, xi)]
        amp *= 0.5
    out -= out.min()
    return out / (out.max() + 1e-6)


sheet = np.zeros((SIZE, SIZE), np.float32)
c = CELL * 0.5
for i in range(GRID * GRID):
    a = i / (GRID * GRID - 1)                 # 0..1 age over the 64 frames
    cell = np.zeros((CELL, CELL), np.float32)
    spread = 4.0 + 22.0 * a                    # cluster expands as it dissipates
    # Irregular cluster: a few ASYMMETRIC, edge-feathered blobs (not a filled round disk). Each blob is
    # stretched along a random axis so the silhouette is ragged + varies per frame (no repeating stamp).
    theta = rng.uniform(0, np.pi)
    ct, st = np.cos(theta), np.sin(theta)
    for _ in range(int(round(9 * (1 - a) + 4))):
        ang, rad = rng.uniform(0, 2 * np.pi), abs(rng.normal(0.0, spread))
        bx, by = c + np.cos(ang) * rad, c + np.sin(ang) * rad
        r = rng.uniform(4.0, 9.0) * (1.0 + 0.6 * a)
        ar = rng.uniform(1.4, 2.6)             # anisotropy -> elongated, not round
        dx, dy = xx - bx, yy - by
        u = (dx * ct + dy * st) / ar           # squash along the blob axis
        v = -dx * st + dy * ct
        cell += np.exp(-((u * u + v * v) / (2.0 * r * r))) * rng.uniform(0.45, 0.95)
    # Fine speckle = individual airborne droplets flung wider (sparser with age).
    for _ in range(int(round(34 * (1 - a) + 8))):
        ang, rad = rng.uniform(0, 2 * np.pi), abs(rng.normal(0.0, spread * 1.35))
        cell += blob(c + np.cos(ang) * rad, c + np.sin(ang) * rad,
                     rng.uniform(0.8, 1.8)) * rng.uniform(0.35, 0.85)
    # TEAR it into lace: multiply by an fBm noise mask (gentler early, biting harder as it ages/aerates).
    tear = fbm((CELL, CELL), 4)
    cell *= np.clip((tear - (0.05 + 0.30 * a)) * 3.0, 0.25, 1.0)
    cell *= (1.0 - a) ** 1.15                  # overall opacity fade with age
    # Soft radial feather so a frame edge never reads as a hard ball / bleeds into its neighbour cell.
    d = np.sqrt((xx - c) ** 2 + (yy - c) ** 2) / (CELL * 0.5)
    cell *= np.clip(1.0 - (d / 0.95) ** 2.4, 0.0, 1.0)
    gx, gy = (i % GRID) * CELL, (i // GRID) * CELL
    sheet[gy:gy + CELL, gx:gx + CELL] = np.clip(cell, 0.0, 1.0)

img = np.dstack([np.full((SIZE, SIZE, 3), 255, np.uint8), (np.clip(sheet, 0, 1) * 255).astype(np.uint8)])
out_dir = "/Users/sunningkim/Desktop/seashield/tools/assets/out/textures"
os.makedirs(out_dir, exist_ok=True)
path = os.path.join(out_dir, "T_SpraySheet.png")
Image.fromarray(img, "RGBA").save(path)
print(f"WROTE {path}  {SIZE}x{SIZE}  {GRID*GRID} frames ({GRID}x{GRID})")
