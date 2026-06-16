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
    """4x4 raised hatches between the deckhouse front and the gun."""
    for row in range(4):
        for col in range(4):
            x = -2.55 + 1.7 * col
            y = 37.4 + 1.4 * row
            parts.append(
                lib.slanted_box(f"vls_{row}{col}", y, y + 1.0, 1.0, 5.8, 6.45,
                                slope_deg=4.0, center_x=x)
            )


def details(lod):
    """Silhouette-enriching superstructure clutter — AEGIS-style SPY radar faces, an
    integrated mast array, CIWS mounts, funnel cap, recessed bridge glass, a foredeck
    breakwater, deck boxes and edge railings. Carries its own materials (Superstructure
    gray, SensorDark) so the UE slot map paints sensors/glass dark against the hull.
    LOD-gated: the fine clutter only on LOD0/1."""
    gray = lib.material("Superstructure", (0.58, 0.60, 0.62))
    dark = lib.material("SensorDark", (0.10, 0.11, 0.13), roughness=0.4, metallic=0.6)
    out = []

    def add(part, mat):
        lib.assign_material(part, mat)
        out.append(part)

    # AEGIS SPY radar faces canted on the deckhouse front corners (port & stbd).
    for sx in (-5.2, 5.2):
        add(lib.slanted_box("spy", 21.5, 27.5, 0.6, 9.8, 13.6, slope_deg=2.0, center_x=sx), dark)
    add(lib.slanted_box("bridge_glass", 33.1, 33.7, 7.2, 13.0, 14.9, slope_deg=12.0), dark)
    add(lib.slanted_box("funnel_cap", -6.5, -1.5, 5.0, 10.8, 12.4, slope_deg=20.0), dark)
    # Integrated mast: slim upper tower + two yardarms + a dark array panel.
    add(lib.slanted_box("mast_tower", 13.5, 16.5, 2.6, 21.0, 28.5, slope_deg=12.0), gray)
    for i, yz in enumerate((24.0, 26.5)):
        add(lib.slanted_box(f"yard{i}", 13.5, 16.5, 9.5, yz, yz + 0.5, slope_deg=0.0), gray)
    add(lib.slanted_box("mast_array", 14.0, 16.0, 3.4, 22.5, 25.5, slope_deg=4.0), dark)
    # CIWS / sensor mounts fore (above the bridge) and aft (atop the hangar).
    add(lib.slanted_box("ciws_fwd", 36.5, 39.5, 3.2, 12.6, 14.6, slope_deg=16.0), gray)
    add(lib.slanted_box("ciws_aft", -16.0, -13.0, 3.2, 11.2, 13.2, slope_deg=16.0), gray)
    add(lib.slanted_box("ciws_aft_dome", -15.4, -13.6, 1.6, 13.2, 14.2, slope_deg=20.0), dark)
    add(lib.slanted_box("breakwater", 40.0, 41.4, 8.4, 6.3, 7.4, slope_deg=32.0), gray)
    if lod <= 1:
        for i, (yy, cx) in enumerate(((8.0, -4.6), (8.0, 4.6), (-22.0, 0.0), (-30.0, 3.5))):
            add(lib.slanted_box(f"deckbox{i}", yy, yy + 2.4, 1.5, 5.4, 6.4, slope_deg=8.0,
                                center_x=cx), gray)
        for sx in (-5.7, 5.7):  # deck-edge railings (thin low bulwark)
            add(lib.slanted_box("rail", -52.0, 47.0, 0.16, 5.5, 6.5, slope_deg=0.0,
                                center_x=sx), gray)
        for i, wx in enumerate((-0.7, 0.0, 0.7)):  # mast whip antennas
            add(lib.slanted_box(f"whip{i}", 14.5, 15.0, 0.16, 28.5, 31.8, slope_deg=1.5,
                                center_x=wx), gray)
    return out


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
        parts.append(lib.slanted_box("turret", 44.0, 49.0, 4.6, 6.3, 8.5, slope_deg=25.0))
    if with_barrel:
        parts.append(lib.cylinder("barrel", 0.16, 6.5, segments, (0.0, 48.2, 7.6),
                                  pitch_deg=8.0))
    if with_vls:
        vls_field(parts)
    elif full:
        # LOD1: one merged plate stands in for the hatch field.
        parts.append(lib.slanted_box("vls_plate", 37.2, 43.2, 6.6, 5.8, 6.45, slope_deg=4.0))
    for part in parts:
        lib.assign_material(part, superstructure)
    if full:
        parts.extend(details(lod))

    ship = lib.join([hull] + parts, f"SM_Frigate_LOD{lod}")
    # Facets stay flat by design: no smooth shading on the hull.
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
