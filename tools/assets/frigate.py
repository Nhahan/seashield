"""Parametric stealth frigate (the own ship / launch platform) — asset_lib.py.

Faceted low-RCS styling in the Incheon/Daegu FFG silhouette class: lofted
chine hull with tumblehome topsides, slanted deckhouse + bridge, integrated
pyramid mast, funnel, hangar with flight deck aft, bow gun and a VLS hatch
field. Facets are the POINT of the design language — the low polygon count
is the aesthetic, not a compromise.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asset_lib as lib

LENGTH = 122.0  # Stern -61 .. bow +61.

# Hull stations: (y, beam, keel_z, chine_z, deck_z). Deck edges pull inward
# (tumblehome) via DECK_BEAM_RATIO.
STATIONS = [
    (-61.0, 11.0, -2.2, -0.2, 5.0),
    (-45.0, 13.0, -3.6, -0.8, 5.0),
    (-15.0, 14.0, -4.0, -1.0, 5.1),
    (15.0, 13.6, -4.0, -0.9, 5.5),
    (38.0, 11.5, -3.6, -0.4, 6.0),
    (52.0, 6.5, -2.6, 0.8, 6.8),
    (61.0, 0.4, -0.2, 3.2, 7.5),
]
DECK_BEAM_RATIO = 0.86

# (cylinder segments, vls hatches, gun barrel, full superstructure) per LOD.
LODS = [(16, True, True, True), (10, False, True, True), (8, False, False, True),
        (6, False, False, False)]


def _smooth(obj):
    """Mark every face smooth — for the genuinely round parts only (cylinders, gun
    barrels). Per-face mesh data, so it survives `join` (unlike the auto-smooth
    modifier). The hull and the slanted-box superstructure stay flat-shaded: the
    facets are the stealth identity, the rounded gear is the counterpoint."""
    for poly in obj.data.polygons:
        poly.use_smooth = True
    return obj


def hull_ring(beam, keel_z, chine_z, deck_z):
    b = beam / 2.0
    d = b * DECK_BEAM_RATIO
    bilge_z = keel_z + 0.25 * (chine_z - keel_z)
    return [
        (0.0, keel_z),
        (-0.62 * b, bilge_z),
        (-b, chine_z),
        (-d, deck_z),
        (d, deck_z),
        (b, chine_z),
        (0.62 * b, bilge_z),
    ]


def vls_field(parts):
    """The Mk41-style strike-length VLS farm — an 8x4 grid of 32 square hatch LIDS over a DARK
    recess floor. naval-AD P3-7c: the prior flush monochrome lids were invisible ("traded 16
    reads-as-something bumps for 32 cells that read as nothing"). Now the lids are LIGHT-gray and
    clearly PROUD (0.24 m) over a SensorDark recess floor, so the gaps read as dark grout lines —
    a real cell grid at silhouette distance. Lids -> parts (gray); the dark base is RETURNED so
    build() can material it dark (the gray parts loop would otherwise overwrite it)."""
    cols = (-1.38, -0.46, 0.46, 1.38)   # 4 across, ~0.72 m lids on ~0.92 m pitch
    for row in range(8):                # 8 deep -> a 32-cell strike module
        y = 36.45 + 0.84 * row
        for col, x in enumerate(cols):
            parts.append(
                lib.slanted_box(f"vls_{row}{col}", y, y + 0.66, 0.72, 6.50, 6.74,
                                slope_deg=5.0, center_x=x)   # proud light lid
            )
    return lib.slanted_box("vls_base", 35.8, 43.6, 4.6, 6.0, 6.50, slope_deg=4.0)  # dark recess floor


def gun_mount(add, gray, dark, lod):
    """The main gun — a faceted angular stealth gun-house (Oto-Melara-127-LW read) with a
    recessed dark mantlet and a TAPERED barrel (thick breech -> thin barrel -> a proud muzzle
    that protrudes well past the house). naval-AD flagged the old single box + stub cylinder as
    'a low-poly Lego turret'; this reads as a nameable 5-inch naval gun. Carries gunmetal/dark
    materials, so it is built here (with details) rather than in the gray-only build() loop."""
    segments = LODS[lod][0]
    # Barbette ring + the faceted shield house (strong inward slope = the low-RCS pyramid),
    # plus a forward-canted snout where the barrel exits.
    add(lib.slanted_box("gun_base", 44.3, 50.2, 5.2, 4.6, 6.0, slope_deg=7.0), gray)
    add(lib.slanted_box("gun_house", 44.7, 49.7, 4.4, 6.0, 7.7, slope_deg=21.0, end_slope_deg=27.0), gray)
    add(lib.slanted_box("gun_snout", 49.0, 50.1, 2.0, 6.55, 7.4, slope_deg=10.0, end_slope_deg=25.0), gray)
    # Dark recessed mantlet (the barrel port) so the gun face reads as an aperture, not a wall.
    add(lib.slanted_box("gun_mantlet", 49.6, 50.05, 1.1, 6.65, 7.25, slope_deg=6.0), dark)
    # Tapered barrel: thick breech -> thin barrel -> proud muzzle brake at the tip.
    add(_smooth(lib.cylinder("gun_breech", 0.17, 3.0, segments, (0.0, 48.6, 6.92), pitch_deg=5.0)), dark)
    add(_smooth(lib.cylinder("gun_barrel", 0.115, 5.6, segments, (0.0, 51.4, 7.16), pitch_deg=5.0)), dark)
    add(_smooth(lib.cylinder("gun_muzzle", 0.165, 0.5, segments, (0.0, 56.8, 7.62), pitch_deg=5.0)), dark)


def details(lod):
    """Silhouette-enriching superstructure clutter — AEGIS-style SPY radar faces, an
    integrated mast array, CIWS mounts, funnel cap, recessed bridge glass, a foredeck
    breakwater, deck boxes and edge railings. Carries its own materials (Superstructure
    gray, SensorDark) so the UE slot map paints sensors/glass dark against the hull.
    LOD-gated: the fine clutter only on LOD0/1."""
    gray = lib.material("Superstructure", (0.58, 0.60, 0.62))
    dark = lib.material("SensorDark", (0.10, 0.11, 0.13), roughness=0.4, metallic=0.6)
    # P3-7.2 (naval-AD): a distinct GUNMETAL family for the weapon mounts so the armament
    # stops reading as hull-colored paint (the slot name maps to UE M_Gunmetal — dark glossy
    # machinery metal between the light hull gray and the black-glass sensors).
    gun = lib.material("Gunmetal", (0.30, 0.31, 0.33), roughness=0.28, metallic=0.8)
    out = []

    def add(part, mat):
        lib.assign_material(part, mat)
        out.append(part)

    # Main gun: faceted house + tapered barrel + dark mantlet (full LODs). The structure
    # (base/house/snout) takes GUNMETAL; the barrel/mantlet stay dark (SensorDark).
    gun_mount(add, gun, dark, lod)

    # AEGIS-style SPY phased-array faces — the signature large flat radar panels, canted on
    # the bridge front corners (port & stbd). A dark array in a gray bezel so it reads as an
    # actual sensor at distance (the old thin strip was invisible — critics: "bare spike").
    for sx in (-4.7, 4.7):
        add(lib.slanted_box("spy_bezel", 24.0, 32.0, 0.9, 9.8, 15.0, slope_deg=4.0, center_x=sx), gray)
        add(lib.slanted_box("spy", 24.8, 31.2, 0.5, 10.2, 14.4, slope_deg=4.0, center_x=sx), dark)
    add(lib.slanted_box("bridge_glass", 33.1, 33.7, 7.2, 13.0, 14.9, slope_deg=12.0), dark)
    add(lib.slanted_box("funnel_cap", -6.5, -1.5, 5.0, 10.8, 12.4, slope_deg=20.0), dark)
    # Integrated mast: slim upper tower + two yardarms + a dark array panel.
    add(lib.slanted_box("mast_tower", 13.5, 16.5, 2.6, 21.0, 28.5, slope_deg=12.0), gray)
    for i, yz in enumerate((24.0, 26.5)):
        add(lib.slanted_box(f"yard{i}", 13.5, 16.5, 9.5, yz, yz + 0.5, slope_deg=0.0), gray)
    add(lib.slanted_box("mast_array", 14.0, 16.0, 3.4, 22.5, 25.5, slope_deg=4.0), dark)
    # Enclosed-mast AESA array faces — flat dark planar panels on the forward + both side faces
    # of the mast tower (naval-AD: the mast 'reads as a bare truss/fin with nothing on it'; a
    # modern integrated mast is a faceted tower carrying flat phased-array faces).
    add(lib.slanted_box("mast_aesa_f", 16.25, 16.5, 2.2, 21.8, 26.2, slope_deg=3.0, end_slope_deg=7.0), dark)
    for sx in (-1.42, 1.42):
        add(lib.slanted_box("mast_aesa_s", 13.8, 16.2, 0.3, 21.8, 25.8, slope_deg=2.0, center_x=sx), dark)
    # CIWS mounts: fwd on the BRIDGE ROOF (a real above-bridge director spot — NOT
    # floating forward of the deckhouse), aft on the hangar roof. Each box mount carries
    # a Phalanx-style ROUND radome drum + a stubby gun barrel, so it reads as an actual
    # close-in weapon system instead of a plain block.
    # CIWS mounts SIT ON real structure (no float): fwd on the bridge roof (spy/bridge tops ~z15),
    # aft fully on the HANGAR roof (hangar = y -44..-14, roof z11.0). Were floating: fwd had an
    # 0.8 m gap above the bridge, aft overhung the hangar edge.
    add(lib.slanted_box("ciws_fwd", 26.5, 29.5, 3.0, 14.6, 16.2, slope_deg=16.0), gun)
    add(lib.slanted_box("ciws_aft", -18.0, -15.0, 3.2, 11.0, 13.0, slope_deg=16.0), gun)
    add(lib.slanted_box("breakwater", 40.0, 41.4, 8.4, 6.3, 7.4, slope_deg=32.0), gray)
    for tag, yy, zt in (("fwd", 28.0, 16.2), ("aft", -16.5, 13.0)):
        add(lib.dome(f"ciws_{tag}_radome", 1.1, 12, (0.0, yy, zt), squash=0.85), gun)
        add(_smooth(lib.cylinder(f"ciws_{tag}_gun", 0.20, 1.7, 8, (0.0, yy + 0.5, zt + 0.5),
                                 pitch_deg=20.0)), dark)
    if lod <= 1:
        for i, (yy, cx) in enumerate(((8.0, -4.6), (8.0, 4.6), (-22.0, 0.0), (-30.0, 3.5))):
            add(lib.slanted_box(f"deckbox{i}", yy, yy + 2.4, 1.5, 5.0, 6.0, slope_deg=8.0,
                                center_x=cx), gray)  # z0 5.4->5.0: seat on deck (deck_z 5.0-5.3), no float gap
        for i, wx in enumerate((-0.7, 0.0, 0.7)):  # mast whip antennas
            add(lib.slanted_box(f"whip{i}", 14.5, 15.0, 0.16, 28.5, 31.8, slope_deg=1.5,
                                center_x=wx), gray)
        _greebles(add, gray, dark, gun)
        # Embarked helo on the aft flight deck (Wildcat-class silhouette) — a big
        # "modern frigate" read at the hero framing. Faceted like the rest of the ship:
        # boxy fuselage + dark canopy, tail boom + fin, and a low-poly main-rotor cross.
        add(lib.slanted_box("helo_body", -55.0, -49.5, 2.2, 5.2, 7.3, slope_deg=9.0), gray)
        add(lib.slanted_box("helo_nose", -49.5, -48.2, 1.9, 5.2, 6.7, slope_deg=18.0), dark)
        add(_smooth(lib.cylinder("helo_tail", 0.22, 4.4, 10, (0.0, -59.2, 6.35))), gray)
        add(lib.slanted_box("helo_fin", -59.4, -58.9, 0.28, 6.2, 8.2, slope_deg=5.0), gray)
        add(lib.slanted_box("helo_hub", -52.9, -52.1, 0.7, 7.3, 7.9, slope_deg=10.0), dark)
        add(lib.slanted_box("helo_rotorA", -52.85, -52.15, 11.4, 7.92, 8.04, slope_deg=0.0), dark)
        add(lib.slanted_box("helo_rotorB", -57.9, -47.1, 0.5, 7.92, 8.04, slope_deg=0.0), dark)
        # Liferaft canisters stowed along the deck edge (both sides) — small ROUND
        # cylinders that break up the long bare sheerline and read as fitted-out.
        for i, yy in enumerate((16.0, 6.0, -4.0)):
            for sx in (-5.3, 5.3):
                add(_smooth(lib.cylinder(f"raft_{i}_{'p' if sx < 0 else 's'}", 0.45, 1.5, 10,
                                         (sx, yy, 5.4))), dark)  # 5.75->5.4: bottom embeds the deck edge, no float
    if lod == 0:
        _railings(add, gray)
    return out


def _greebles(add, gray, dark, gun):
    """Angular 'fitted-out warship' clutter — keeps the faceted language but adds the
    equipment density a real frigate carries, so the superstructure stops reading as a
    bare greybox: round SATCOM/nav radomes on the mast platform, funnel exhaust uptakes,
    a foredeck windlass + capstans + mooring bollards, an aft RAM box launcher, and a
    RHIB on a port-side cradle."""
    # SATCOM / nav radomes on the mast platform (round, against the faceted mast).
    for sx in (-2.1, 2.1):
        add(lib.dome("satcom", 0.85, 10, (sx, 14.0, 18.6), squash=0.9), gray)
    add(lib.dome("navradome", 0.55, 10, (0.0, 16.2, 19.4), squash=1.0), gray)
    # Funnel exhaust uptakes poking from the cap — round pipes break the funnel block.
    for sx in (-1.6, 1.6):
        add(lib.vcylinder("uptake", 0.32, 1.5, 8, (sx, -4.2, 12.2)), dark)
    # Foredeck ground tackle: a windlass box, two capstan drums, mooring bollards.
    add(lib.slanted_box("windlass", 52.5, 54.5, 2.2, 6.9, 7.4, slope_deg=6.0), gun)  # P3-7.S2: machinery = gunmetal contrast (reads as a fitting, not hull)
    for sx in (-1.6, 1.6):
        add(lib.vcylinder("capstan", 0.45, 0.8, 10, (sx, 55.6, 7.0)), gun)
    for (yy, sx) in ((-6.0, -5.2), (-6.0, 5.2), (-25.0, -4.8), (-25.0, 4.8),
                     (-42.0, -4.5), (-42.0, 4.5)):
        add(lib.vcylinder("bollard", 0.18, 0.7, 6, (sx, yy, 5.4), smooth=False), gun)
    # Aft RAM/SeaRAM box launcher — SIT IT ON THE HANGAR ROOF (hangar y -44..-14, roof z11.0).
    # Was at y -11.5..-8 in the OPEN-DECK gap between hangar and funnel -> floated ~6.5 m in the air.
    add(lib.slanted_box("ram_box", -25.0, -21.0, 3.0, 11.0, 12.8, slope_deg=20.0,
                        end_slope_deg=10.0), dark)
    # RHIB rescue boat on a port-side cradle amidships (a small V-hull on a dark cradle).
    add(lib.slanted_box("rhib", 1.5, 7.0, 1.5, 5.7, 6.5, slope_deg=-14.0, center_x=-4.9), gray)
    add(lib.slanted_box("rhib_cradle", 2.0, 6.5, 1.7, 5.4, 5.8, slope_deg=4.0, center_x=-4.9), dark)


def _railings(add, gray):
    """Open guardrails along the midship main-deck edge — thin vertical stanchions + three
    horizontal lifelines, port & stbd. The single biggest 'real warship' silhouette cue up
    close (the old solid bulwark strip read as a blank wall). Thin geometry, so LOD0 only:
    it aliases at distance and the lower LODs simply omit it."""
    y0, y1 = -45.0, 14.0
    n = 18
    for sx in (-5.6, 5.6):
        for i in range(n):
            yy = y0 + (y1 - y0) * i / (n - 1)
            add(lib.slanted_box("stanchion", yy - 0.045, yy + 0.045, 0.09, 5.5, 6.65,
                                slope_deg=0.0, center_x=sx), gray)
        for lz in (6.0, 6.32, 6.62):
            add(lib.slanted_box("lifeline", y0, y1, 0.05, lz - 0.025, lz + 0.025,
                                slope_deg=0.0, center_x=sx), gray)


def build(lod):
    segments, with_vls, with_barrel, full = LODS[lod]

    hull = lib.loft("hull", [(y, hull_ring(b, k, c, d)) for (y, b, k, c, d) in STATIONS])
    lib.assign_material(hull, lib.material("HullGray", (0.45, 0.47, 0.50)))
    lib.assign_material(hull, lib.material("DeckDark", (0.30, 0.31, 0.33), roughness=0.85),
                        faces_where=lambda c: c.z >= 4.95)

    superstructure = lib.material("Superstructure", (0.58, 0.60, 0.62))
    parts = []
    parts.append(lib.slanted_box("deckhouse", 6.0, 36.0, 12.4, 5.4, 12.5, slope_deg=11.0))
    parts.append(lib.slanted_box("bridge", 24.0, 34.0, 9.5, 12.5, 15.8, slope_deg=12.0))
    parts.append(lib.slanted_box("mast", 12.0, 20.0, 7.0, 12.5, 21.0, slope_deg=14.5))
    if full:
        parts.append(lib.slanted_box("funnel", -8.0, 0.0, 8.4, 5.4, 10.8, slope_deg=9.0))
        parts.append(lib.slanted_box("hangar", -44.0, -14.0, 11.6, 5.0, 11.0, slope_deg=8.0))
        # The main gun (faceted house + tapered barrel + dark mantlet) is built inside
        # details() so it can carry its own gunmetal/dark materials, like the other armament.
    vls_base = None
    if with_vls:
        vls_base = vls_field(parts)
    elif full:
        # LOD1: one merged plate stands in for the hatch field.
        parts.append(lib.slanted_box("vls_plate", 37.2, 43.2, 6.6, 5.8, 6.45, slope_deg=4.0))
    for part in parts:
        lib.assign_material(part, superstructure)
    if vls_base is not None:
        # Dark recess floor under the proud light lids (the grid's dark grout lines).
        lib.assign_material(vls_base, lib.material("SensorDark", (0.10, 0.11, 0.13),
                                                   roughness=0.4, metallic=0.6))
        parts.append(vls_base)
    if full:
        parts.extend(details(lod))

    ship = lib.join([hull] + parts, f"SM_Frigate_LOD{lod}")
    # P3-7 (quality > faceted-identity): chamfer the edges on the hero LODs so the angular
    # stealth silhouette reads as MACHINED metal (edges catch light) rather than raw CG boxes.
    # LOD0/1 only — distant LODs stay cheap.
    if lod <= 1:
        # P3-A — naval-AD 5 ranked this the #1 fix: the blocks read as flat-shaded boxes with
        # hard 90° edges and NO lit chamfer. 0.09 m was sub-pixel at hero distance. Widen to
        # 0.18 m so every edge carries a defined flat chamfer that catches a specular highlight
        # line ("fabricated steel", not papercraft). clamp_overlap protects thin greebles/railings.
        lib.bevel_all(ship, offset=0.18, segments=1)
    if lod == 0:
        # Cavity AO baked to a vertex-color attribute: block junctions, deck-edge recesses, and
        # the base of every vertical face DARKEN by occlusion — the geometry value-break that stops
        # the hull/superstructure reading as flat 'putty monolith' (naval-AD). The UE material
        # multiplies base color by a tempered remap of this. Hero LOD0 only (subdivides ~4x for the
        # AO gradient; distant LODs stay cheap and read white = unaffected).
        lib.cavity_ao(ship, cuts=1, samples=16)
    return ship


def main():
    out = os.path.join(lib.OUT_DIR, "frigate")
    for lod in range(len(LODS)):
        lib.reset_scene()
        ship = build(lod)
        triangles = sum(len(p.vertices) - 2 for p in ship.data.polygons)
        print(f"SM_Frigate_LOD{lod}: {triangles} tris")
        lib.export_fbx(ship, os.path.join(out, f"SM_Frigate_LOD{lod}.fbx"))
        if lod == 0:
            lib.render_previews(os.path.join(out, "preview_frigate"), LENGTH * 0.8)


if __name__ == "__main__":
    main()
