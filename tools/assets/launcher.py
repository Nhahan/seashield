"""Parametric trainable rocket launcher (deck mount, 4x4 tube cluster).

Three sub-meshes so UE can articulate the mount: SM_LauncherBase (fixed),
SM_LauncherMount (trains in azimuth) and SM_LauncherTubes (elevates).
Exported per LOD into one FBX each, named accordingly. Tubes are modeled at
zero elevation along +Y; the client pitches the tube assembly.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asset_lib as lib

TUBE_RADIUS = 0.10
TUBE_LENGTH = 3.2
TUBE_GRID = 4  # 4x4 = the 16-round salvo the experiments stress.
TUBE_PITCH = 0.26  # Center-to-center spacing.

LODS = [(16, True), (10, True), (8, False), (6, False)]


def build(lod):
    segments, tubes_individually = LODS[lod]
    steel = lib.material("LauncherSteel", (0.40, 0.42, 0.44), roughness=0.5)

    base = lib.slanted_box("SM_LauncherBase", -1.2, 1.2, 2.4, 0.0, 0.45, slope_deg=18.0)
    mount = lib.slanted_box("SM_LauncherMount", -0.95, 0.95, 1.9, 0.45, 1.05, slope_deg=12.0)

    cluster_parts = []
    span = TUBE_PITCH * (TUBE_GRID - 1)
    if tubes_individually:
        for row in range(TUBE_GRID):
            for col in range(TUBE_GRID):
                x = -span / 2.0 + TUBE_PITCH * col
                z = 1.35 + TUBE_PITCH * row
                cluster_parts.append(
                    lib.cylinder(f"tube_{row}{col}", TUBE_RADIUS, TUBE_LENGTH, segments,
                                 (x, -TUBE_LENGTH / 2.0, z))
                )
    else:
        # Distant LODs: the cluster reads as one slab.
        cluster_parts.append(
            lib.slanted_box("tubes", -TUBE_LENGTH / 2.0, TUBE_LENGTH / 2.0,
                            span + 2.2 * TUBE_RADIUS, 1.35 - TUBE_RADIUS,
                            1.35 + span + TUBE_RADIUS, slope_deg=0.0)
        )
    # Side cheek plates carrying the cluster on the mount (up to mid-bundle).
    for side in (-1.0, 1.0):
        cluster_parts.append(
            lib.slanted_box(f"cheek_{int(side)}", -0.75, 0.75, 0.12, 1.05,
                            1.35 + span / 2.0,
                            slope_deg=0.0, center_x=side * (span / 2.0 + 0.28))
        )
    tubes = lib.join(cluster_parts, f"SM_LauncherTubes_LOD{lod}")

    for obj, name in ((base, "SM_LauncherBase"), (mount, "SM_LauncherMount")):
        obj.name = f"{name}_LOD{lod}"
        obj.data.name = obj.name
    for obj in (base, mount, tubes):
        lib.assign_material(obj, steel)
    return base, mount, tubes


def main():
    out = os.path.join(lib.OUT_DIR, "launcher")
    for lod in range(len(LODS)):
        lib.reset_scene()
        base, mount, tubes = build(lod)
        triangles = sum(
            len(p.vertices) - 2 for o in (base, mount, tubes) for p in o.data.polygons
        )
        print(f"SM_Launcher_LOD{lod}: {triangles} tris")
        for obj in (base, mount, tubes):
            lib.export_fbx(obj, os.path.join(out, f"{obj.name}.fbx"))
        if lod == 0:
            lib.render_previews(os.path.join(out, "preview_launcher"), TUBE_LENGTH * 2.4)


if __name__ == "__main__":
    main()
