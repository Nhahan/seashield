"""Cinematic ocean mesh — a flat high-density subdivided grid that the Gerstner-WPO material
(M_Ocean) displaces in-engine into rolling swells. This is the from-scratch realistic-water
rebuild (2026-06-18): the prior SLW/CineOcean experiment meshes are gone; this is a clean,
dense grid sized for maximum quality (fps ignored). Render-only — buoyancy still samples the
hidden Water-plugin ocean's Gerstner waves, so the simulation/determinism is untouched.

  ~1.2 km patch (the apply spawns at 0.02 actor scale, mirroring the proven FBX m<->cm 100x
  import), ~1.5 m UE vertex spacing (800 segments) so the swell crests resolve crisply near
  camera; the fine chop is the material's normal map, not geometry.

Run headlessly (Blender 5.x):
    blender -b -P tools/assets/ocean.py
Outputs tools/assets/out/ocean/SM_Ocean_LOD0.fbx (gitignored, like the other FBX); import with
client/SeaShield/Tools/import_ocean.py.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asset_lib as lib

SIZE_CM = 60000.0   # Blender units; the apply spawns at 0.02 scale -> ~1.2 km UE patch
SEGMENTS = 800      # ~1.5 m UE vertex spacing after import+scale — crisp displaced swell crests


def main():
    lib.reset_scene()
    grid = lib.subdivided_grid("SM_Ocean_LOD0", SIZE_CM, SEGMENTS)
    nverts = len(grid.data.vertices)
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "ocean")
    lib.export_fbx(grid, os.path.join(out, "SM_Ocean_LOD0.fbx"))
    print(f"SM_Ocean_LOD0: {nverts} verts ({SEGMENTS}x{SEGMENTS} grid)")


if __name__ == "__main__":
    main()
