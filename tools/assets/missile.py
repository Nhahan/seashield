"""Parametric anti-ship missile (the simulator's target) — see asset_lib.py.

Generic sea-skimmer in the Exocet/Haeseong silhouette class: cylinder body,
tangent-ogive radome, boattail, four mid-body wings and four tail control
fins in X configuration. All dimensions are parameters; every LOD is rebuilt
from them with its own segment budget.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asset_lib as lib

# Airframe parameters (meters).
LENGTH = 5.2
RADIUS = 0.175
NOSE_LENGTH = 0.9
TAIL_RADIUS = 0.14
BOATTAIL_LENGTH = 0.3
WING_THICKNESS = 0.020
FIN_THICKNESS = 0.018

# (body segments, ogive samples, with tail fins) per LOD.
LODS = [(48, 12, True), (24, 8, True), (12, 5, True), (8, 3, False)]

HALF = LENGTH / 2.0
NOSE_BASE = HALF - NOSE_LENGTH

# Planform outlines in (forward, up), root edges sunk into the body so the
# join needs no boolean. Wings mid-body, control fins at the tail.
WING_OUTLINE = [(-0.50, 0.10), (0.15, 0.10), (0.00, 0.52), (-0.35, 0.52)]
FIN_OUTLINE = [(-2.55, 0.10), (-2.15, 0.10), (-2.25, 0.42), (-2.50, 0.42)]


def body_profile(ogive_samples):
    profile = [
        (-HALF, 0.0),
        (-HALF, TAIL_RADIUS),
        (-HALF + BOATTAIL_LENGTH, RADIUS),
        (NOSE_BASE, RADIUS),
    ]
    for y_offset, r in lib.tangent_ogive_profile(NOSE_LENGTH, RADIUS, ogive_samples)[1:]:
        profile.append((NOSE_BASE + y_offset, r))
    return profile


def build(lod):
    segments, ogive_samples, with_fins = LODS[lod]
    parts = [lib.revolve("body", body_profile(ogive_samples), segments)]
    parts += lib.radial_copies(lib.fin("wing", WING_OUTLINE, WING_THICKNESS), 4, offset_deg=45.0)
    if with_fins:
        parts += lib.radial_copies(lib.fin("fin", FIN_OUTLINE, FIN_THICKNESS), 4, offset_deg=45.0)
    missile = lib.join(parts, f"SM_Missile_LOD{lod}")

    lib.assign_material(missile, lib.material("MissileBody", (0.55, 0.57, 0.58)))
    radome = lib.material("Radome", (0.24, 0.25, 0.27), roughness=0.4, metallic=0.0)
    lib.assign_material(missile, radome, faces_where=lambda c: c.y > NOSE_BASE + 0.15)
    lib.shade_smooth(missile)
    return missile


def main():
    out = os.path.join(lib.OUT_DIR, "missile")
    for lod in range(len(LODS)):
        lib.reset_scene()
        missile = build(lod)
        triangles = sum(len(p.vertices) - 2 for p in missile.data.polygons)
        print(f"SM_Missile_LOD{lod}: {triangles} tris")
        lib.export_fbx(missile, os.path.join(out, f"SM_Missile_LOD{lod}.fbx"))
        if lod == 0:
            lib.render_previews(os.path.join(out, "preview_missile"), LENGTH)


if __name__ == "__main__":
    main()
