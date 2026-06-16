"""Procedural PBR detail maps for the hard-surface assets (charter §9 P5 — assets
as code, not binaries). numpy + PIL, fully deterministic (seeded). Output is a
pair of SEAMLESS tiling maps applied TRIPLANAR in-engine (world-space, no UVs):

  T_ShipDetail_N.png    tangent-space normal — plate offsets, panel/weld seams,
                        micro surface tooth.
  T_ShipDetail_RAO.png  R = roughness (painted-metal base + weathering breakup +
                        rougher seams + rust patches), G = ambient occlusion
                        (dark in the seams), B = dirt/rust mask (albedo tint).

Seamlessness: the fractal fields are built by FFT 1/f filtering of white noise
(periodic by construction); the panel grids use periods that divide the texture
size. So the map tiles with no visible seam under triplanar projection.

Run headlessly (host python):
    python3 tools/assets/textures.py
Outputs to tools/assets/out/textures/ (gitignored, like the FBX); import into UE
with client/SeaShield/Tools/import_textures.py.
"""

import os

import numpy as np
from PIL import Image

SIZE = 1024
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "textures")


def _seamless_fbm(size, exponent, seed):
    """FFT 1/f^exponent noise — periodic, ~zero-mean unit-std. Higher exponent =
    smoother/larger features."""
    rng = np.random.default_rng(seed)
    white = rng.standard_normal((size, size))
    spectrum = np.fft.fft2(white)
    fy = np.fft.fftfreq(size)[:, None]
    fx = np.fft.fftfreq(size)[None, :]
    freq = np.sqrt(fx * fx + fy * fy)
    freq[0, 0] = 1.0
    spectrum *= 1.0 / (freq ** exponent)
    img = np.fft.ifft2(spectrum).real
    img -= img.mean()
    std = img.std()
    return img / std if std > 1e-9 else img


def _norm01(a):
    a = a - a.min()
    m = a.max()
    return a / m if m > 1e-9 else a


def _seam(size, period, halfwidth):
    """1.0 on grid lines spaced `period` (both axes), 0 elsewhere. Periodic when
    period divides size — distance to the nearest multiple of period."""
    c = np.arange(size)
    d = c % period
    d = np.minimum(d, period - d)  # 0 on the line, rises to period/2 between
    line = (d <= halfwidth).astype(np.float64)
    gx = np.broadcast_to(line[None, :], (size, size))
    gy = np.broadcast_to(line[:, None], (size, size))
    return np.clip(gx + gy, 0.0, 1.0)


def _plate_offsets(size, period, seed):
    """Per-plate constant height offset (plating that doesn't sit perfectly
    flush). Tileable because the plate grid aligns with the texture edges."""
    n = size // period
    rng = np.random.default_rng(seed)
    offs = rng.uniform(-1.0, 1.0, (n, n))
    return np.kron(offs, np.ones((period, period)))


def _height_to_normal(h, strength):
    """Periodic-wrap gradient -> tangent-space normal, encoded 0..1 (OpenGL +Y)."""
    gx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5
    gy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * 0.5
    nx = -gx * strength
    ny = -gy * strength
    nz = np.ones_like(h)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    nx *= inv
    ny *= inv
    nz *= inv
    return np.stack([nx * 0.5 + 0.5, ny * 0.5 + 0.5, nz * 0.5 + 0.5], axis=-1)


def _sprite_smoke(size=512, seed=99):
    """Turbulent, billowing smoke puff (RGBA) for the billboard particle stream.
    The key vs a clean radial blob: the falloff RADIUS is warped by low-freq noise
    so the silhouette is LUMPY (cauliflower lobes), not a disc — so when many puffs
    overlap down a trail they read as one turbulent column instead of a stack of
    round cotton balls. Multi-octave density carves internal billows; the rim is
    eroded to wisps. RGB is a mid-grey albedo (the lit material colours it by age),
    A is the puff."""
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    c = (size - 1) / 2.0
    # Density is deliberately LOW-frequency (two broad octaves only): the lumpy look
    # must come from the SILHOUETTE, not from fine internal detail — a high-freq
    # octave here, repeated across hundreds of overlapping puffs, reads as grain
    # ('cottage cheese'). Smooth interior + lumpy edge => puffs MELD, not granulate.
    f1 = _norm01(_seamless_fbm(size, 2.2, seed))
    f2 = _norm01(_seamless_fbm(size, 3.4, seed + 7))
    density = 0.55 * f1 + 0.45 * f2
    # Perturb the radius itself -> lumpy, asymmetric silhouette (the cauliflower).
    warp = _norm01(_seamless_fbm(size, 1.5, seed + 31)) - 0.5
    r = np.sqrt((xx - c) ** 2 + (yy - c) ** 2) / (size * 0.5)
    r = r - 0.34 * warp
    radial = np.clip(1.0 - r, 0.0, 1.0) ** 1.05    # soft core falloff
    alpha = np.clip(radial * (0.18 + 1.35 * density), 0.0, 1.0)
    alpha = np.clip((alpha - 0.08) / 0.92, 0.0, 1.0) ** 1.1   # erode to a soft wispy rim
    val = np.clip(0.70 + 0.18 * (density - 0.5), 0.40, 0.95)
    rgb = np.stack([val, val, val * 1.03], axis=-1)
    return (np.clip(np.dstack([rgb, alpha]), 0, 1) * 255).astype(np.uint8)


def _sprite_flash(size=256):
    """Muzzle/launch flash (RGBA) — a hot white core with orange radial rays
    (a multi-point star), emissive. RGB is the flash colour, A its shape."""
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    c = (size - 1) / 2.0
    dx, dy = xx - c, yy - c
    r = np.sqrt(dx * dx + dy * dy) / (size * 0.5)
    ang = np.arctan2(dy, dx)
    spikes = (0.5 + 0.5 * np.cos(ang * 9.0)) ** 3      # 9-point star
    core = np.clip(1.0 - r, 0.0, 1.0) ** 2.2
    rays = np.clip(1.0 - r, 0.0, 1.0) * spikes
    alpha = np.clip(core * 1.25 + 0.55 * rays, 0.0, 1.0)
    white = np.array([1.0, 0.95, 0.85])
    orange = np.array([1.0, 0.52, 0.16])
    mix = core[..., None]
    rgb = orange[None, None, :] * (1.0 - mix) + white[None, None, :] * mix
    return (np.clip(np.dstack([rgb, alpha]), 0, 1) * 255).astype(np.uint8)


def _ship_detail(seeds):
    """One variation of the plate/weld/weathering detail maps -> (normal, rao, dirt_mask).
    The plate GRID stays regular (real plating is regular), but the SEEDED fields —
    plate-offset, micro-tooth, weathering, rust — differ per `seeds`. A 2nd variation
    blended in-engine by a low-freq macro mask therefore breaks the visible ~6.5 m
    triplanar tile repeat (the same rust blotch no longer recurs every tile). P2-5.
    1024 px over a 4 m tile => 256 px/m: 1 m plates, 0.25 m sub-panels."""
    major, minor = 256, 64  # divide SIZE => seamless
    seam_major = _seam(SIZE, major, 2)
    seam_minor = _seam(SIZE, minor, 1)
    # Height -> normal: subtle architectural plating, NOT corrugation.
    height = np.zeros((SIZE, SIZE))
    height += 0.10 * _plate_offsets(SIZE, major, seeds["plate"])  # plates not perfectly flush
    height -= 0.45 * seam_major                                   # recessed plate seams
    height -= 0.16 * seam_minor                                   # finer panel lines
    height += 0.018 * _norm01(_seamless_fbm(SIZE, 1.5, seeds["tooth"]))  # faint micro tooth
    normal = _height_to_normal(height, strength=1.2)
    # Roughness / AO / dirt.
    rough = np.full((SIZE, SIZE), 0.42)                           # painted-metal base
    rough += 0.22 * (_norm01(_seamless_fbm(SIZE, 2.3, seeds["weather"])) - 0.5) * 2.0
    rough += 0.16 * seam_major + 0.09 * seam_minor
    dirt = _norm01(_seamless_fbm(SIZE, 2.8, seeds["dirt"]))
    dirt_mask = np.clip((dirt - 0.68) / 0.18, 0.0, 1.0)           # sparse rust/dirt patches
    rough += 0.22 * dirt_mask
    rough = np.clip(rough, 0.06, 0.92)
    ao = np.ones((SIZE, SIZE))
    ao -= 0.55 * seam_major + 0.30 * seam_minor
    ao -= 0.18 * dirt_mask
    ao = np.clip(ao, 0.0, 1.0)
    return normal, np.stack([rough, ao, dirt_mask], axis=-1), dirt_mask


def build():
    os.makedirs(OUT_DIR, exist_ok=True)

    # TWO baked variations of the ship detail (suffix "" = original seeds, "2" = a
    # different seed set). The material blends them by a macro mask so the hull's
    # weathering/rust no longer recurs every triplanar tile (anti-tiling, P2-5).
    variants = [
        ("",  {"plate": 7,   "tooth": 11,  "weather": 23,  "dirt": 53}),
        ("2", {"plate": 907, "tooth": 311, "weather": 223, "dirt": 653}),
    ]
    for suffix, seeds in variants:
        normal, rao, dirt_mask = _ship_detail(seeds)
        n_path = os.path.join(OUT_DIR, f"T_ShipDetail_N{suffix}.png")
        rao_path = os.path.join(OUT_DIR, f"T_ShipDetail_RAO{suffix}.png")
        Image.fromarray((np.clip(normal, 0, 1) * 255).astype(np.uint8), "RGB").save(n_path)
        Image.fromarray((np.clip(rao, 0, 1) * 255).astype(np.uint8), "RGB").save(rao_path)
        print(f"textures: wrote {n_path} + {rao_path} "
              f"(variant{suffix or '0'}, dirt {100.0 * (dirt_mask > 0.1).mean():.1f}%)")

    smoke_path = os.path.join(OUT_DIR, "T_Smoke.png")
    flash_path = os.path.join(OUT_DIR, "T_Flash.png")
    Image.fromarray(_sprite_smoke(), "RGBA").save(smoke_path)
    Image.fromarray(_sprite_flash(), "RGBA").save(flash_path)
    print(f"textures: wrote {smoke_path}")
    print(f"textures: wrote {flash_path}")


if __name__ == "__main__":
    build()
