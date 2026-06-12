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
