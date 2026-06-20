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
# geo-A+ item 5/6: the bow shoulder (y38->61) was 3 stations over 23 m — beam
# collapsed 11.5->0.4 m in two jumps, breaking the bow into hard origami facets.
# Added 2 interpolated stations (y45.5, y56.5) so the bow reads as a FAIR (still
# faceted) curve. The stern (y-61) gets a near-duplicate transom ring (y-60.0)
# pulled to a flat athwart line so the stern caps as a SQUARE TRANSOM flush to
# the helo deck, not a raked n-gon.
STATIONS = [
    (-61.0, 10.6, -2.0, -0.1, 5.0),   # transom inner ring (square stern face)
    (-60.0, 11.0, -2.2, -0.2, 5.0),   # transom outer ring -> flat vertical face
    (-45.0, 13.0, -3.6, -0.8, 5.0),
    (-15.0, 14.0, -4.0, -1.0, 5.1),
    (15.0, 13.6, -4.0, -0.9, 5.5),
    (38.0, 11.5, -3.6, -0.4, 6.0),
    (45.5, 9.6, -3.2, -0.05, 6.35),   # bow shoulder fairing
    (52.0, 6.5, -2.6, 0.8, 6.8),
    (56.5, 3.6, -1.4, 1.9, 7.15),     # bow shoulder fairing
    (61.0, 0.4, -0.2, 3.2, 7.5),
]
DECK_BEAM_RATIO = 0.86
# Deck half-width FLOOR (m): amidships the deck is tumblehome (narrower than the chine), but at the
# fine bow `beam*DECK_BEAM_RATIO` collapses toward zero -> the forecastle deck became a flat-shaded
# KNIFE RIDGE that mirrored the bright sky as a hard white centerline specular line at high view
# angles. Flooring the deck half-width keeps a real (fine) forecastle deck and, where the floor
# exceeds beam*ratio, FLARES the topside over the sharp waterline cutwater — the realistic warship
# bow flare. Only the bowmost station (beam<~1.6 m) is affected; midships is unchanged.
MIN_DECK_HALF = 0.8

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
    d = max(b * DECK_BEAM_RATIO, MIN_DECK_HALF)  # floored at the bow -> fine forecastle deck + flare, no knife ridge
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
    recess floor. geo-A+ item 3a: widened the athwart span to ~6 m (cols at +-0.69/+-2.07 on a
    0.92 m pitch) so the field fills the foredeck like a real strike module. Lids ~0.82 m on the
    0.92 m pitch (thin ~0.10 m grout) and only modestly PROUD (0.13 m, z 6.50->6.63) over a
    SensorDark recess floor — the prior 0.24 m proudness read as battlement 'crenellations/teeth'
    at grazing angle; thin grout + modest proudness = a populated hatch field, not teeth. Lids ->
    parts (gray); the dark base is RETURNED so build() can material it dark."""
    cols = (-2.07, -0.69, 0.69, 2.07)   # 4 across on 0.92 m pitch -> ~6 m span
    for row in range(8):                # 8 deep -> a 32-cell strike module
        y = 36.35 + 0.92 * row
        for col, x in enumerate(cols):
            parts.append(
                # geo-r2 item 2: proud height 0.13 -> 0.20 m (z6.50->6.70) so the lids read
                # as a populated hatch grid at GRAZING angle. The dense/wide field keeps this
                # from re-reading as battlement crenellation (that came from 0.24 on a sparse
                # field) — thin ~0.10 m grout + 0.20 proud = a real cell grid.
                lib.slanted_box(f"vls_{row}{col}", y, y + 0.82, 0.82, 6.50, 6.70,
                                slope_deg=4.0, center_x=x)   # proud light lid, thin grout
            )
    return lib.slanted_box("vls_base", 35.6, 44.6, 5.6, 6.0, 6.50, slope_deg=4.0)  # dark recess floor


def gun_mount(add, gray, dark, lod):
    """The main gun — a faceted angular stealth gun-house (Oto-Melara-127-LW read) with a
    recessed dark mantlet and a TAPERED barrel (thick breech -> thin barrel -> a proud muzzle
    that protrudes well past the house). naval-AD flagged the old single box + stub cylinder as
    'a low-poly Lego turret'; this reads as a nameable 5-inch naval gun. Carries gunmetal/dark
    materials, so it is built here (with details) rather than in the gray-only build() loop."""
    segments = LODS[lod][0]
    # geo-A+ item 3b: the 5-inch mount upscaled ~25% (wider barbette/house, taller shield)
    # and the barrel protrusion lengthened so the gun is the DOMINANT foredeck signature,
    # not a small nub. Barbette ring + faceted shield house (strong inward slope = the
    # low-RCS pyramid), plus a forward-canted snout where the barrel exits.
    add(lib.slanted_box("gun_base", 43.6, 50.6, 6.5, 4.6, 6.2, slope_deg=7.0), gray)
    add(lib.slanted_box("gun_house", 44.1, 50.0, 5.5, 6.2, 8.3, slope_deg=21.0, end_slope_deg=27.0), gray)
    add(lib.slanted_box("gun_snout", 49.2, 50.5, 2.5, 6.7, 7.8, slope_deg=10.0, end_slope_deg=25.0), gray)
    # Dark recessed mantlet (the barrel port) so the gun face reads as an aperture, not a wall.
    add(lib.slanted_box("gun_mantlet", 50.0, 50.55, 1.4, 6.85, 7.6, slope_deg=6.0), dark)
    # Tapered barrel: thick breech -> thin barrel -> proud muzzle brake at the tip, pushed
    # further forward (muzzle out to ~y59.6) so it dominates the foredeck.
    add(_smooth(lib.cylinder("gun_breech", 0.20, 3.4, segments, (0.0, 49.2, 7.18), pitch_deg=5.0)), dark)
    add(_smooth(lib.cylinder("gun_barrel", 0.135, 6.6, segments, (0.0, 52.6, 7.46), pitch_deg=5.0)), dark)
    add(_smooth(lib.cylinder("gun_muzzle", 0.19, 0.6, segments, (0.0, 59.2, 8.02), pitch_deg=5.0)), dark)


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
    add(lib.slanted_box("bridge_glass", 33.1, 33.7, 7.2, 14.0, 16.0, slope_deg=12.0), dark)
    # geo-A+ item 2: a PROMINENT single raked funnel rising from the boat-deck gap
    # (between the 01-level aft end y4 and the hangar fwd end y-14). The old funnel was a
    # squat detached block with tiny uptake pipes; this is a wide, strongly-raked exhaust
    # mass with a dark grille cap, so the ship visibly has an uptake. Built in two stacked
    # stages: a wide base emerging from the deck, then a raked upper trunk pulled FORWARD
    # (end_slope canting the fore/aft faces) so it leans like a real stack. center_y rake
    # is achieved by stepping each stage forward.
    add(lib.slanted_box("funnel_base", -9.0, 1.0, 6.6, 5.4, 9.2, slope_deg=9.0, end_slope_deg=4.0), gray)
    add(lib.slanted_box("funnel_trunk", -7.4, 1.6, 5.4, 9.2, 14.4, slope_deg=13.0, end_slope_deg=30.0), gray)
    # Dark exhaust grille cap recessed into the funnel top (the actual uptake aperture).
    add(lib.slanted_box("funnel_cap", -4.2, 0.4, 3.6, 14.2, 14.9, slope_deg=8.0), dark)
    # Integrated mast: slim upper tower + two yardarms + a dark array panel.
    add(lib.slanted_box("mast_tower", 13.5, 16.5, 2.6, 21.0, 28.5, slope_deg=12.0), gray)
    # geo-A+ item 7 / geo-r2 item 4: yardarms thickened (0.5 -> 0.7 -> 0.85 m vertical
    # section) so they don't sub-pixel-shimmer against the sky/glitter at hero distance.
    for i, yz in enumerate((24.0, 26.5)):
        add(lib.slanted_box(f"yard{i}", 13.5, 16.5, 9.5, yz, yz + 0.85, slope_deg=0.0), gray)
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
    # Foredeck breakwater (spray deflector): moved to y33.8->35.2, just FORWARD of the
    # deckhouse front (y30) and AFT of the widened VLS field (which starts y36.35), the
    # real naval position — it deflects spray before the VLS/superstructure and no longer
    # sits in the middle of the lids. geo-A+ item 3a side-effect.
    add(lib.slanted_box("breakwater", 33.8, 35.2, 8.4, 6.3, 7.4, slope_deg=32.0), gray)
    for tag, yy, zt in (("fwd", 28.0, 16.2), ("aft", -16.5, 13.0)):
        add(lib.dome(f"ciws_{tag}_radome", 1.1, 12, (0.0, yy, zt), squash=0.85), gun)
        add(_smooth(lib.cylinder(f"ciws_{tag}_gun", 0.20, 1.7, 8, (0.0, yy + 0.5, zt + 0.5),
                                 pitch_deg=20.0)), dark)
    if lod <= 1:
        for i, (yy, cx) in enumerate(((8.0, -4.6), (8.0, 4.6), (-22.0, 0.0), (-30.0, 3.5))):
            add(lib.slanted_box(f"deckbox{i}", yy, yy + 2.4, 1.5, 5.0, 6.0, slope_deg=8.0,
                                center_x=cx), gray)  # z0 5.4->5.0: seat on deck (deck_z 5.0-5.3), no float gap
        for i, wx in enumerate((-0.75, 0.0, 0.75)):  # mast whip antennas
            # geo-A+ item 7 / geo-r2 item 4: whip section 0.16 -> 0.25 -> 0.32 m so they
            # don't sub-pixel-shimmer against the sky. Widened spacing too (the thicker
            # whips would otherwise merge). LOD0/1 only (inside the lod<=1 block).
            add(lib.slanted_box(f"whip{i}", 14.5, 15.0, 0.32, 28.5, 31.8, slope_deg=1.5,
                                center_x=wx), gray)
        _greebles(add, gray, dark, gun)
        _deck_clutter(add, gray, dark, gun)
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
    bare greybox: round SATCOM/nav radomes on the mast platform, a foredeck windlass +
    capstans + bollards + anchor hawse nubs, two SSM anti-ship missile banks amidships,
    an aft RAM box launcher, and a RHIB with a davit crane on a port-side cradle."""
    # SATCOM / nav radomes on the mast platform (round, against the faceted mast).
    for sx in (-2.1, 2.1):
        add(lib.dome("satcom", 0.85, 10, (sx, 14.0, 18.6), squash=0.9), gray)
    add(lib.dome("navradome", 0.55, 10, (0.0, 16.2, 19.4), squash=1.0), gray)
    # geo-A+ item 2: the tiny 1.5 m uptake pipes are dropped — the prominent raked funnel
    # (funnel_base/trunk/cap in details()) now reads as the exhaust on its own.
    # Foredeck ground tackle: a windlass box, two capstan drums, mooring bollards.
    add(lib.slanted_box("windlass", 52.5, 54.5, 2.2, 6.9, 7.4, slope_deg=6.0), gun)  # P3-7.S2: machinery = gunmetal contrast (reads as a fitting, not hull)
    for sx in (-1.6, 1.6):
        add(lib.vcylinder("capstan", 0.45, 0.8, 10, (sx, 55.6, 7.0)), gun)
    # geo-A+ item 8: anchor/hawse-pipe nubs on the bow flare (port+stbd) — short dark
    # cylinders set into the flare where the anchor stows, so the bow reads as fitted-out.
    # Placed at y52 (beam 6.5, deck half-width ~2.8) so they sit ON the flare, not off it.
    for sx in (-1.8, 1.8):
        add(_smooth(lib.cylinder("hawse", 0.28, 1.1, 8, (sx, 51.6, 7.0), pitch_deg=-18.0)), dark)
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
    # geo-A+ item 8: a small RHIB davit/crane inboard of the boat — a vertical post plus a
    # thin box boom cantilevered OUTBOARD over the cradle, so the RHIB reads as a
    # launch-and-recovery rig. The boom spans X (width) from the post out past the boat.
    add(_smooth(lib.vcylinder("davit_post", 0.18, 3.2, 8, (-3.2, 4.2, 5.6))), gun)
    add(lib.slanted_box("davit_boom", 3.9, 4.5, 2.6, 8.4, 8.7, slope_deg=0.0, center_x=-4.5), gun)
    # geo-A+ item 3c: 8x SSM-700K Haeseong anti-ship missile canisters amidships — TWO
    # angled banks of 4, mounted on raised sponson pedestals FLANKING the funnel (port &
    # stbd, outboard of the funnel footprint x~3.3, inboard of the deck edge ~5.95), in the
    # boat-deck zone between the deckhouse and the hangar. A real Daegu/Incheon carries these
    # (Incheon mounts Haeseong abreast the funnel) and they were entirely absent — this is
    # the fire-control story. Each bank = a gray sponson pedestal + a canted gunmetal cradle
    # + 4 dark canister tubes raked UP ~18deg and splayed across the bank.
    for side, sgn in (("p", -1.0), ("s", 1.0)):
        bx = sgn * 4.1                                  # bank center, abreast the funnel (geo-r2: pulled in 0.5 m)
        # geo-r2 item 1: pedestal + cradle lowered ~1.0 m (cradle top z8.2->7.2) and the
        # bank pulled inboard 0.5 m so the canted canisters clear the funnel WITHOUT the
        # block perching proud on the sheerline (it read toylike on the broadside). Still
        # abreast the funnel, still canted up 18deg.
        add(lib.slanted_box(f"ssm_ped_{side}", -10.5, -3.5, 2.4, 5.2, 6.2,
                            slope_deg=8.0, end_slope_deg=14.0, center_x=bx), gray)
        add(lib.slanted_box(f"ssm_cradle_{side}", -10.2, -3.8, 2.6, 6.2, 7.2,
                            slope_deg=6.0, end_slope_deg=24.0, center_x=bx), gun)
        for t in range(4):
            tx = bx + sgn * (0.48 * (t - 1.5))          # 4 tubes splayed across the bank
            add(_smooth(lib.cylinder(f"ssm_{side}{t}", 0.31, 5.0, 8,
                                     (tx, -10.0, 7.2), pitch_deg=18.0)), dark)


def _deck_clutter(add, gray, dark, gun):
    """geo-r2 item 3: working-warship deck density so a top-down / orbit frame isn't bare
    plate. Faceted low-poly, LOD0/1 only (called from inside the lod<=1 block). Adds: a
    flight-deck tie-down pad grid + recovery track, RAS (replenishment-at-sea) posts port
    & stbd, aft cable reels, deck-edge cleats along the sheerline, and extra liferaft
    canisters on the side decks. Everything seats ON the deck (deck_z ~5.0-5.3 amidships/aft,
    flight deck ~5.0) — no float."""
    # Flight-deck tie-down pad grid (y-60..-46): a 4x5 lattice of small flush DARK pads — the
    # padeye grid a helo lashes to. Fills the single biggest empty plate (the flight deck).
    for r in range(5):
        yy = -59.0 + 3.1 * r
        for sx in (-3.6, -1.2, 1.2, 3.6):
            add(lib.slanted_box(f"tiedown_{r}", yy, yy + 0.5, 0.5, 5.0, 5.12,
                                slope_deg=0.0, center_x=sx), dark)
    # Recovery-assist track + a touchdown bullseye ring stand-in: two thin dark rails down
    # the flight-deck centerline so the landing spot reads as a marked deck, not blank.
    for sx in (-0.7, 0.7):
        add(lib.slanted_box("recovery_rail", -60.0, -47.0, 0.18, 5.0, 5.10,
                            slope_deg=0.0, center_x=sx), dark)
    # RAS (replenishment-at-sea) receiving posts — a tall post + a small highline head, port
    # & stbd at the break of the deckhouse (the real RAS station spot). Gunmetal machinery.
    for sx in (-5.3, 5.3):
        add(_smooth(lib.vcylinder("ras_post", 0.22, 3.6, 8, (sx, -2.0, 5.2))), gun)
        add(lib.slanted_box("ras_head", -2.6, -1.4, 0.7, 8.5, 9.2, slope_deg=10.0, center_x=sx), gun)
    # Cable/hose reels on the HANGAR ROOF aft (hangar y-44..-14, roof z11.0), clear of the
    # RAM box (y-25..-21) and aft CIWS (y-18..-15) — round drums on a dark axle box, working
    # gear that breaks the open hangar-roof plate.
    for sx in (-3.0, 3.0):
        add(_smooth(lib.cylinder("cable_reel", 0.62, 1.3, 10, (sx - 0.65, -38.0, 11.0))), gun)
        add(lib.slanted_box("reel_axle", -38.9, -37.1, 1.7, 11.0, 11.5, slope_deg=4.0, center_x=sx), dark)
    # Deck-edge cleats / chocks along the sheerline (small dark nubs, both sides) — the fine
    # fittings that make the long deck edge read as worked, not extruded.
    for yy in (24.0, 12.0, 0.0, -12.0, -30.0, -38.0):
        for sx in (-5.5, 5.5):
            add(lib.slanted_box("cleat", yy - 0.3, yy + 0.3, 0.5, 5.0, 5.35,
                                slope_deg=12.0, center_x=sx), dark)
    # Extra liferaft canisters on the side decks (more than the original 3/side) — round
    # white drums that populate the long side-deck walkways.
    for yy in (20.0, 10.0, -8.0, -20.0):
        for sx in (-5.45, 5.45):
            add(_smooth(lib.cylinder(f"raft2_{'p' if sx < 0 else 's'}", 0.42, 1.3, 10,
                                     (sx, yy, 5.35))), gray)


def _railings(add, gray):
    """Open guardrails along the midship main-deck edge — thin vertical stanchions + three
    horizontal lifelines, port & stbd. The single biggest 'real warship' silhouette cue up
    close (the old solid bulwark strip read as a blank wall). Thin geometry, so LOD0 only:
    it aliases at distance and the lower LODs simply omit it."""
    # geo-r2 item 4: stanchions 0.09 -> 0.14 m section and lifelines 0.05 -> 0.10 m so the
    # rail run stops sub-pixel-shimmering at hero distance (the loudest remaining 'real-time
    # game' tell). Fewer stanchions (18 -> 15) keeps the thicker posts from reading as a
    # picket wall. Railings are already LOD0-ONLY, so the thicker section costs nothing at
    # range (lower LODs omit the rail entirely).
    y0, y1 = -45.0, 14.0
    n = 15
    for sx in (-5.6, 5.6):
        for i in range(n):
            yy = y0 + (y1 - y0) * i / (n - 1)
            add(lib.slanted_box("stanchion", yy - 0.07, yy + 0.07, 0.14, 5.5, 6.7,
                                slope_deg=0.0, center_x=sx), gray)
        for lz in (6.05, 6.38, 6.68):
            add(lib.slanted_box("lifeline", y0, y1, 0.10, lz - 0.05, lz + 0.05,
                                slope_deg=0.0, center_x=sx), gray)


def build(lod):
    segments, with_vls, with_barrel, full = LODS[lod]

    hull = lib.loft("hull", [(y, hull_ring(b, k, c, d)) for (y, b, k, c, d) in STATIONS])
    lib.assign_material(hull, lib.material("HullGray", (0.45, 0.47, 0.50)))
    lib.assign_material(hull, lib.material("DeckDark", (0.30, 0.31, 0.33), roughness=0.85),
                        faces_where=lambda c: c.z >= 4.95)

    superstructure = lib.material("Superstructure", (0.58, 0.60, 0.62))
    parts = []
    # geo-A+ item 1/4: the deckhouse was ONE 30 m flat-roofed box (the "barge/monolith"
    # read). Rebuild it as a deliberate STEPPED STAIRCASE that tiers up bow-ward, each
    # tier stepped IN and UP from the one aft of it, and each tier's half-width kept <=
    # the LOCAL deck half-beam (deck edge = beam/2 * 0.86) so the base sits inside the
    # sheerline (no proud overhang). Half-beam: y6~5.93, y15~5.85, y28~5.4, y38~4.95.
    #   01-level: widest, lowest, runs aft (boat deck) -> 02-level: stepped in/up ->
    #   bridge structure: narrower/tallest forward -> mast. The FRONT face steps, not a wall.
    parts.append(lib.slanted_box("deck01_aft", 4.0, 18.0, 11.4, 5.3, 9.6, slope_deg=10.0))   # 01-level boat deck (wide, low)
    parts.append(lib.slanted_box("deck01_fwd", 18.0, 30.0, 10.4, 5.4, 11.2, slope_deg=10.0))  # 01-level steps up
    parts.append(lib.slanted_box("deck02", 22.0, 33.0, 9.0, 11.2, 13.6, slope_deg=11.0))       # 02-level, stepped in + up
    parts.append(lib.slanted_box("bridge", 25.5, 34.0, 7.8, 13.6, 16.2, slope_deg=12.0))       # bridge structure, narrowest + tallest fwd
    parts.append(lib.slanted_box("mast", 12.0, 20.0, 7.0, 9.6, 21.0, slope_deg=14.5))
    if full:
        parts.append(lib.slanted_box("hangar", -44.0, -14.0, 11.6, 5.0, 11.0, slope_deg=8.0))
        # The main gun (faceted house + tapered barrel + dark mantlet), the prominent
        # funnel, and the dark hangar door are built inside details() so they can carry
        # their own gunmetal/dark materials, like the other armament.
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
