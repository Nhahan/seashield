# Focused reimport of ONLY SM_Frigate (LOD0..3) + reassign its material slots from
# the materials already in the project — for fast frigate.py geometry iterations
# without recreating every material (which would delete/recreate MI_SeaOcean and
# dangle the level's ocean reference). Run like import_assets.py:
#   UnrealEditor SeaShield.uproject -nullrhi -unattended \
#     -ExecCmds="py /abs/path/client/SeaShield/Tools/reimport_frigate.py"
import os
import re

import unreal

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SRC = os.path.join(REPO_ROOT, "tools", "assets", "out", "frigate")
DEST = "/Game/SeaShield/Meshes"
LOD_TEMP = DEST + "/_LodTemp"
MAT_DIR = "/Game/SeaShield/Materials"


def import_sm(fbx, dest_path, name):
    o = unreal.FbxImportUI()
    o.import_mesh = True
    o.import_as_skeletal = False
    o.import_animations = False
    o.import_materials = False
    o.import_textures = False
    o.static_mesh_import_data.combine_meshes = True
    o.static_mesh_import_data.generate_lightmap_u_vs = True
    o.static_mesh_import_data.auto_generate_collision = True
    # Keep the baked cavity-AO vertex colors (frigate.py cavity_ao) — the material multiplies
    # base color by them. Default IGNORE would drop them and the hull would read flat again.
    o.static_mesh_import_data.vertex_color_import_option = unreal.VertexColorImportOption.REPLACE
    t = unreal.AssetImportTask()
    t.filename = fbx
    t.destination_path = dest_path
    t.destination_name = name
    t.automated = True
    t.replace_existing = True
    t.save = True
    t.options = o
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([t])
    a = unreal.load_asset(f"{dest_path}/{name}")
    if a is None:
        raise RuntimeError(f"import failed {fbx}")
    return a


def main():
    mesh_sub = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    base = import_sm(os.path.join(SRC, "SM_Frigate_LOD0.fbx"), DEST, "SM_Frigate")
    for lod in range(1, 4):
        src = import_sm(os.path.join(SRC, f"SM_Frigate_LOD{lod}.fbx"), LOD_TEMP, f"SM_Frigate_LOD{lod}")
        mesh_sub.set_lod_from_static_mesh(base, lod, src, 0, True)
    unreal.EditorAssetLibrary.save_loaded_asset(base)
    unreal.EditorAssetLibrary.delete_directory(LOD_TEMP)

    hull = unreal.load_asset(f"{MAT_DIR}/M_NavalHull")
    gray = unreal.load_asset(f"{MAT_DIR}/M_NavalGray")
    dark = unreal.load_asset(f"{MAT_DIR}/M_SensorDark")
    gunmetal = unreal.load_asset(f"{MAT_DIR}/M_Gunmetal")
    if None in (hull, gray, dark, gunmetal):
        raise RuntimeError("missing materials — run setup_materials.py first")
    slot_map = {"HullGray": hull, "DeckDark": gray, "Superstructure": gray,
                "SensorDark": dark, "Gunmetal": gunmetal}
    slots = base.static_materials
    for i in range(len(slots)):
        nm = str(slots[i].get_editor_property("imported_material_slot_name"))
        b = re.sub(r"_\d+$", "", nm)
        base.set_material(i, slot_map.get(nm) or slot_map.get(b, hull))
    unreal.EditorAssetLibrary.save_loaded_asset(base)
    unreal.log(f"SeaShieldReimport: SM_Frigate slots={[str(s.get_editor_property('imported_material_slot_name')) for s in slots]}")
    unreal.log("SeaShieldReimport: done")


if __name__ == "__main__":
    try:
        main()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldReimport: FAILED\n{traceback.format_exc()}")
    finally:
        unreal.SystemLibrary.quit_editor()
