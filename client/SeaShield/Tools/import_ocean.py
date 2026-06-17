# Import the cinematic ocean grid -> /Game/SeaShield/Meshes/SM_Ocean (from-scratch water rebuild).
#   UnrealEditor SeaShield.uproject -nullrhi -unattended -ExecCmds="py .../import_ocean.py"
import os

import unreal

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
FBX = os.path.join(REPO_ROOT, "tools", "assets", "out", "ocean", "SM_Ocean_LOD0.fbx")
DEST = "/Game/SeaShield/Meshes"


def main():
    if not os.path.isfile(FBX):
        raise RuntimeError(f"missing FBX (run tools/assets/ocean.py with Blender): {FBX}")
    options = unreal.FbxImportUI()
    options.import_mesh = True
    options.import_as_skeletal = False
    options.import_materials = False
    options.import_textures = False
    options.static_mesh_import_data.combine_meshes = True
    options.static_mesh_import_data.generate_lightmap_u_vs = True
    options.static_mesh_import_data.auto_generate_collision = False  # flat ocean, no collision
    task = unreal.AssetImportTask()
    task.filename = FBX
    task.destination_path = DEST
    task.destination_name = "SM_Ocean"
    task.automated = True
    task.replace_existing = True
    task.save = True
    task.options = options
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    asset = unreal.load_asset(f"{DEST}/SM_Ocean")
    if asset is None:
        raise RuntimeError("SM_Ocean import failed")
    unreal.log(f"SeaShieldOcean: SM_Ocean imported ({asset.get_num_vertices(0)} verts)")
    unreal.log("SeaShieldOcean: DONE")


_state = {"go": True, "h": None}


def _deferred(dt):
    try:
        if _state["go"]:
            _state["go"] = False
            main()
            unreal.unregister_slate_post_tick_callback(_state["h"])
            unreal.SystemLibrary.quit_editor()
    except Exception:  # noqa: BLE001
        import traceback
        unreal.log_error(f"SeaShieldOcean: FAILED\n{traceback.format_exc()}")
        unreal.unregister_slate_post_tick_callback(_state["h"])
        unreal.SystemLibrary.quit_editor()


if __name__ == "__main__":
    _state["h"] = unreal.register_slate_post_tick_callback(_deferred)
