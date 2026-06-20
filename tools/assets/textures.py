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

SIZE = 2048  # P3-7: 2K detail maps — 2x texel density so plate/weld/weathering reads up close
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


def _strake(size, period, halfwidth, butt_mult=3):
    """Horizontal plate STRAKES (rows at `period`) + SPARSE vertical butt-joints (every
    butt_mult plates). Real welded hull plating runs as long horizontal strakes with occasional
    vertical butts — NOT the square `_seam` grid, whose equal H+V crossings emboss into a dot/waffle
    lattice under a strong detail-normal (techart A+ gate). Periodic when period|size and butt|size."""
    c = np.arange(size)
    dh = c % period
    dh = np.minimum(dh, period - dh)
    rows = (dh <= halfwidth).astype(np.float64)            # dense horizontal strake lines
    bp = period * butt_mult
    dv = c % bp
    dv = np.minimum(dv, bp - dv)
    cols = (dv <= halfwidth).astype(np.float64)            # sparse vertical butt joints
    gy = np.broadcast_to(rows[:, None], (size, size))      # horizontal (varies by row)
    gx = np.broadcast_to(cols[None, :], (size, size))      # vertical (varies by col)
    return np.clip(gy + 0.7 * gx, 0.0, 1.0)


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


def _water_ripple(size=1024):
    """Seamless tiling tangent-space NORMAL for the open-ocean micro-surface (TRACK W,
    render-eng gate): the DEFAULT_LIT custom ocean has NO high-frequency normal of its own
    (only the plugin's coarse Gerstner bands), so the sea reads matte/glassy with no sun
    glitter. This bakes a multi-octave water-ripple height -> normal that the ocean material
    samples at THREE world scales (capillary ~1.75 m, chop ~7 m, swell ~27 m), each panned in
    a different direction, summed and faded — the facet field the sun/sky reflection needs to
    scatter into a scintillating glitter path. FFT 1/f octaves => periodic/seamless under the
    world-XY planar projection (no visible tile beat).

    The look target is WATER, not metal: smooth rolling swell carrying finer wind chop and a
    crisp capillary tooth — i.e. a few octaves of increasingly fine, decreasingly strong noise,
    NOT a single rough field. Mild anisotropy (wind-driven) by stretching the height along X."""
    # Three height octaves: large smooth swell + mid chop + fine capillary tooth. Exponents
    # high->low = smooth->sharp; weights large->small so the surface rolls then ripples.
    swell = _seamless_fbm(size, 2.4, 1201)
    chop = _seamless_fbm(size, 1.7, 1213)
    capi = _seamless_fbm(size, 1.1, 1229)
    # Wind anisotropy: bias toward crest lines perpendicular to wind by softening the X gradient
    # of the swell (stretches features along X) — keeps it from reading as isotropic "noise".
    swell = 0.5 * (swell + np.roll(swell, 1, axis=1))
    h = 1.00 * swell + 0.55 * chop + 0.30 * capi
    h = (h - h.mean()) / (h.std() + 1e-9)
    # Moderate relief — strong enough that the in-graph octave weights have tilt to scale, but
    # the material's overall strength/Fresnel/depth fades are the real intensity controls.
    # Strong relief: real sea slopes reach ±15-25°; a timid ±5° normal is invisible against the
    # navy base + broad sky reflection (w3 proved it). Bake ample tilt headroom and let the
    # material's in-graph strength/Fresnel/depth fades dial it back down.
    normal = _height_to_normal(h, strength=5.0)
    return (np.clip(normal, 0, 1) * 255).astype(np.uint8)


def _hull_markings(w=1024, h=256):
    """Single-stamp (NON-tiling) hull-markings stencil for the human-scale decal pass (naval-AD
    #1 lever: the ship reads as a 30 m model, not a 122 m warship, for lack of any human-frequency
    reference). R channel = white-paint mask projected ONCE onto the bow topsides in-engine
    (world-Y -> U over the forward hull, world-Z -> V over the topside band, CLAMP). Carries: the
    pennant/hull number, draft-mark ladder at the stem, and a few hatch/door outlines — the marks
    that anchor scale. White on black; drawn procedurally so it stays 'assets as code'."""
    from PIL import ImageDraw, ImageFont
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)

    def _font(px):
        for p in ("/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                  "/System/Library/Fonts/Helvetica.ttc",
                  "/System/Library/Fonts/SFNS.ttf"):
            try:
                return ImageFont.truetype(p, px)
            except Exception:  # noqa: BLE001
                continue
        try:
            return ImageFont.load_default(size=px)
        except Exception:  # noqa: BLE001
            return ImageFont.load_default()

    # Pennant number on the forward hull (U≈0.30..0.52), large. "81" = plausible frigate hull no.
    # Bigger + brighter so it reads at the ~110 m hero distance, not just the close quarter.
    big = _font(182)
    d.text((int(w * 0.30), int(h * 0.12)), "81", fill=252, font=big)
    # Draft-mark ladder at the bow stem (U≈0.05): numbers 2..8 climbing, with tick rows.
    small = _font(34)
    for i, num in enumerate((2, 4, 6, 8)):
        y = int(h * 0.80) - i * int(h * 0.20)
        d.text((int(w * 0.045), y - 16), str(num), fill=210, font=small)
        d.rectangle([int(w * 0.085), y - 3, int(w * 0.105), y + 3], fill=210)  # tick
    # A few hatch/door outlines on the after topside (U≈0.62..0.92), thin stencil rectangles.
    for ux, uy, uw, uh in ((0.64, 0.40, 0.05, 0.34), (0.74, 0.42, 0.07, 0.30),
                           (0.85, 0.38, 0.06, 0.40)):
        x0, y0 = int(w * ux), int(h * uy)
        x1, y1 = int(w * (ux + uw)), int(h * (uy + uh))
        d.rectangle([x0, y0, x1, y1], outline=150, width=3)
    arr = np.asarray(img).astype(np.float64) / 255.0
    rgb = np.stack([arr, arr, arr], axis=-1)  # grayscale -> RGB (R used as the mask in-engine)
    return (np.clip(rgb, 0, 1) * 255).astype(np.uint8)


def _deck_markings(w=1024, h=130):
    """Top-down deck-markings stencil (S2 polish — naval-AD gap #3: the main deck reads as an
    empty plane from the hero 3/4 above-angle). R = white-paint mask projected DOWN onto the deck
    in-engine (world-Y -> U bow..stern, world-X -> V beam; CLAMP). Carries the iconic from-above
    cues: an aft helo landing circle + 'H', deck-edge non-skid dashes, and a couple of foredeck
    hatch outlines. Placed at the OPEN deck ends (helo aft, hatches forward) to clear the amidships
    superstructure (which shares this material), so no Z-mask is needed. 8:1 aspect ~ the deck."""
    from PIL import ImageDraw, ImageFont
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)
    # aft helo landing circle + H (U~0.93 = stern flight deck), centered on the beam
    cx, cy, r = int(w * 0.93), h // 2, int(h * 0.34)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=235, width=4)
    try:
        hf = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial Bold.ttf", int(h * 0.42))
    except Exception:  # noqa: BLE001
        hf = ImageFont.load_default()
    d.text((cx - int(h * 0.15), cy - int(h * 0.24)), "H", fill=235, font=hf)
    # deck-edge non-skid dashes along the length, both edges
    for v in (int(h * 0.14), int(h * 0.86)):
        x = int(w * 0.16)
        while x < int(w * 0.97):
            d.rectangle([x, v - 2, x + 22, v + 2], fill=140)
            x += 40
    # a few foredeck hatch outlines (U~0.03..0.12 = forward of the superstructure, open foredeck)
    for ux in (0.04, 0.08, 0.12):
        x0, y0 = int(w * ux), int(h * 0.34)
        d.rectangle([x0, y0, x0 + 26, y0 + int(h * 0.32)], outline=150, width=3)
    arr = np.asarray(img).astype(np.float64) / 255.0
    return (np.clip(np.stack([arr, arr, arr], axis=-1), 0, 1) * 255).astype(np.uint8)


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
    # P3-7e — the dense quilted grid was PLATE DENSITY, not the weld bead: 8 major plates/texture
    # at a 220-420 cm tile = ~0.3-0.5 m plates, far too fine (real hull plating is ~1-2 m). 4 major
    # plates/texture => ~1 m plates at a 420 cm tile, ~1 m on the superstructure at 400 cm — clean
    # architectural panel lines at the right density instead of a rivet/quilt field.
    major, minor = 512, 128  # divide SIZE => seamless. 4 major plates/texture (~1 m at 420 cm)
    # P3-7b — naval-AD read the prior bake as "soft rounded pillow / orange-peel", NOT flat:
    # broad gentle undulation with no dead-flat plate field between the joins. Real welded warship
    # steel is DEAD-FLAT plate + a SHARP narrow deep butt-joint recess + a THIN proud weld bead.
    # Re-author to that: kill the broad doming (plate offsets ~0, fine high-freq tooth only) and
    # let crisp seams carry all the signal.
    seam_major = _strake(SIZE, major, 3)  # A+: horizontal STRAKES + sparse vertical butts (was a square grid -> dot/waffle); reads as plated steel, not studs
    seam_minor = _seam(SIZE, minor, 2)   # hairline sub-panel
    height = np.zeros((SIZE, SIZE))
    # P3-7e — the raised WELD-BEAD ring (P3-7c/d) framed every plate into a "button/rivet" tile,
    # reading as over-textured/quilted at close range. Drop the bead entirely: a clean THIN
    # recessed seam LINE on a dead-flat plate field reads as architectural PANEL LINES, not
    # buttons. Moderate strength so the lines catch the rake without embossing the whole surface.
    height -= 0.70 * seam_major                                  # P3-7.4: deeper plate-butt recess (0.50->0.70) so seams cast a micro-shadow under the raked key
    # A+/techart: minor sub-panel grid REMOVED from the NORMAL — crossing the major grid it embossed
    # a polka-dot/quilt lattice once the material strength stack (tex 3.2 x material 1.7) amplified it.
    # Sub-panels survive only as faint VALUE lines in RAO/AO below; the MAJOR plate lines now carry
    # 100% of the normal relief -> crisp architectural panel lines, not dots.
    height += 0.006 * _norm01(_seamless_fbm(SIZE, 0.7, seeds["tooth"]))  # FINE tooth only (sandpaper)
    normal = _height_to_normal(height, strength=3.2)            # A+: major-only relief, pushed 2.8->3.2 so the clean plate lines read as plated steel at the hero distance
    # Roughness / AO / dirt.
    rough = np.full((SIZE, SIZE), 0.42)                           # painted-metal base
    rough += 0.22 * (_norm01(_seamless_fbm(SIZE, 2.3, seeds["weather"])) - 0.5) * 2.0
    rough += 0.16 * seam_major + 0.04 * seam_minor  # P3-7g: fainter minor grid (de-dot)
    # P3-7: multi-scale weathering — a broad stain field + a finer rust octave so the grime
    # reads at multiple distances (weathered warship, not flat paint). Direction-free (triplanar
    # can't keep streaks vertical across faces — that belongs in a world-Z material layer).
    # Sparse threshold keeps it from going blotchy/filthy.
    dirt = _norm01(_seamless_fbm(SIZE, 2.8, seeds["dirt"]))
    fine = _norm01(_seamless_fbm(SIZE, 1.9, seeds["dirt"] + 31))
    dirt_mask = np.clip((np.clip(0.68 * dirt + 0.32 * fine, 0.0, 1.0) - 0.66) / 0.20, 0.0, 1.0)
    rough += 0.22 * dirt_mask
    rough = np.clip(rough, 0.06, 0.92)
    ao = np.ones((SIZE, SIZE))
    ao -= 0.55 * seam_major + 0.12 * seam_minor  # P3-7g: major plate seams dominate the AO; minor faint (de-dot)
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

    ripple_path = os.path.join(OUT_DIR, "T_WaterRipple_N.png")
    Image.fromarray(_water_ripple(), "RGB").save(ripple_path)
    print(f"textures: wrote {ripple_path} (ocean micro-normal, 3-scale)")

    mark_path = os.path.join(OUT_DIR, "T_HullMarkings.png")
    Image.fromarray(_hull_markings(), "RGB").save(mark_path)
    print(f"textures: wrote {mark_path} (hull-number/draft/hatch decal stencil)")

    deck_path = os.path.join(OUT_DIR, "T_DeckMarkings.png")
    Image.fromarray(_deck_markings(), "RGB").save(deck_path)
    print(f"textures: wrote {deck_path} (deck helo-circle/non-skid/hatch stencil)")

    smoke_path = os.path.join(OUT_DIR, "T_Smoke.png")
    flash_path = os.path.join(OUT_DIR, "T_Flash.png")
    Image.fromarray(_sprite_smoke(), "RGBA").save(smoke_path)
    Image.fromarray(_sprite_flash(), "RGBA").save(flash_path)
    print(f"textures: wrote {smoke_path}")
    print(f"textures: wrote {flash_path}")


if __name__ == "__main__":
    build()
