# Headless import of the procedural assets (tools/assets/out FBX) into the UE
# project, LOD chains included — run via the full editor (the asset-import
# completion path touches Slate, so -run=pythonscript commandlets crash):
#   UnrealEditor SeaShield.uproject -nullrhi -unattended \
#     -ExecCmds="py client/SeaShield/Tools/import_assets.py"
# The script quits the editor itself when done.
# Idempotent: re-running replaces the meshes in place (replace_existing), so
# the Blender edit-render loop extends straight into the engine ("물리부터
# 에셋까지 전부 코드", charter §11).
import os

import unreal

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SRC_ROOT = os.path.join(REPO_ROOT, "tools", "assets", "out")
DEST = "/Game/SeaShield/Meshes"
LOD_TEMP = DEST + "/_LodTemp"
LOD_COUNT = 4  # The generators bake LOD0..3 (client-design.md §7).

MESHES = {
    "frigate": ["SM_Frigate"],
    "launcher": ["SM_LauncherBase", "SM_LauncherMount", "SM_LauncherTubes"],
    "missile": ["SM_Missile"],
    "rocket": ["SM_Rocket"],
}


def import_static_mesh(fbx_path, dest_path, dest_name):
    options = unreal.FbxImportUI()
    options.import_mesh = True
    options.import_as_skeletal = False
    options.import_animations = False
    options.import_materials = False  # Materials are authored in-engine.
    options.import_textures = False
    options.static_mesh_import_data.combine_meshes = True
    options.static_mesh_import_data.generate_lightmap_u_vs = True
    options.static_mesh_import_data.auto_generate_collision = True

    task = unreal.AssetImportTask()
    task.filename = fbx_path
    task.destination_path = dest_path
    task.destination_name = dest_name
    task.automated = True
    task.replace_existing = True
    task.save = True
    task.options = options
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    asset = unreal.load_asset(f"{dest_path}/{dest_name}")
    if asset is None:
        raise RuntimeError(f"import failed: {fbx_path}")
    return asset


def main():
    mesh_subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    imported = []
    for group, names in MESHES.items():
        for name in names:
            base_fbx = os.path.join(SRC_ROOT, group, f"{name}_LOD0.fbx")
            if not os.path.isfile(base_fbx):
                raise RuntimeError(f"missing FBX (run the tools/assets generators): {base_fbx}")
            base = import_static_mesh(base_fbx, DEST, name)
            for lod in range(1, LOD_COUNT):
                lod_fbx = os.path.join(SRC_ROOT, group, f"{name}_LOD{lod}.fbx")
                source = import_static_mesh(lod_fbx, LOD_TEMP, f"{name}_LOD{lod}")
                mesh_subsystem.set_lod_from_static_mesh(base, lod, source, 0, True)
            unreal.EditorAssetLibrary.save_loaded_asset(base)
            bounds = base.get_bounding_box()
            size = bounds.max - bounds.min
            unreal.log(
                f"SeaShieldAssets: {name} lods={base.get_num_lods()} "
                f"size=({size.x:.0f}, {size.y:.0f}, {size.z:.0f}) cm"
            )
            imported.append(name)
    # The temp LOD sources only exist to donate render data — drop them.
    unreal.EditorAssetLibrary.delete_directory(LOD_TEMP)
    unreal.log(f"SeaShieldAssets: imported {len(imported)} meshes: {', '.join(imported)}")


if __name__ == "__main__":
    try:
        main()
    except Exception:  # noqa: BLE001 — surface the traceback, then still quit.
        import traceback

        unreal.log_error(f"SeaShieldAssets: FAILED\n{traceback.format_exc()}")
    finally:
        unreal.SystemLibrary.quit_editor()
