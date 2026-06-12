"""Parametric unguided interceptor rocket (the simulator's ordnance).

Plain spin-stabilized airframe: cylinder, short ogive, four fixed tail fins.
Deliberately simpler than the missile — it has no seeker and no wings, which
is the whole research premise (charter §2.4).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asset_lib as lib

LENGTH = 2.4
RADIUS = 0.09
NOSE_LENGTH = 0.32
TAIL_RADIUS = 0.075
BOATTAIL_LENGTH = 0.12
FIN_THICKNESS = 0.012

LODS = [(32, 10, True), (16, 6, True), (10, 4, True), (6, 3, False)]

HALF = LENGTH / 2.0
NOSE_BASE = HALF - NOSE_LENGTH
FIN_OUTLINE = [(-1.18, 0.05), (-0.88, 0.05), (-0.94, 0.26), (-1.16, 0.26)]


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
    if with_fins:
        parts += lib.radial_copies(lib.fin("fin", FIN_OUTLINE, FIN_THICKNESS), 4,
                                   offset_deg=45.0)
    rocket = lib.join(parts, f"SM_Rocket_LOD{lod}")
    lib.assign_material(rocket, lib.material("RocketBody", (0.36, 0.40, 0.36)))
    lib.assign_material(rocket, lib.material("RocketNose", (0.20, 0.20, 0.22)),
                        faces_where=lambda c: c.y > NOSE_BASE + 0.05)
    lib.shade_smooth(rocket)
    return rocket


def main():
    out = os.path.join(lib.OUT_DIR, "rocket")
    for lod in range(len(LODS)):
        lib.reset_scene()
        rocket = build(lod)
        triangles = sum(len(p.vertices) - 2 for p in rocket.data.polygons)
        print(f"SM_Rocket_LOD{lod}: {triangles} tris")
        lib.export_fbx(rocket, os.path.join(out, f"SM_Rocket_LOD{lod}.fbx"))
        if lod == 0:
            lib.render_previews(os.path.join(out, "preview_rocket"), LENGTH)


if __name__ == "__main__":
    main()
