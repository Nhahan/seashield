# Phase-6b Path A: build the custom DEFAULT_LIT ocean (M_SeaOceanCustom + MI_SeaOceanCustom)
# and point a THROWAWAY L_RangeCustom map's ocean at it for capture testing. NEVER writes
# L_Range — the real beauty map only gets the custom ocean once the look is critic-approved.
#   UnrealEditor SeaShield.uproject -nullrhi -unattended \
#     -ExecCmds="py /abs/.../client/SeaShield/Tools/apply_custom.py"
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unreal

import setup_materials as sm

CUSTOM_MAP = "/Game/SeaShield/Maps/L_RangeCustom"
MAT_DIR = "/Game/SeaShield/Materials"


def main():
    sm.make_sea_ocean_custom()
    unreal.log("SeaShieldCustom: M_SeaOceanCustom + MI_SeaOceanCustom built")

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    by_label = {a.get_actor_label(): a for a in actors.get_all_level_actors()}
    ocean = by_label.get("Ocean")
    mat = unreal.load_asset(f"{MAT_DIR}/MI_SeaOceanCustom")
    if ocean is None or mat is None:
        raise RuntimeError("missing Ocean actor or MI_SeaOceanCustom")
    body = ocean.get_water_body_component()
    for prop in ("water_material", "water_static_mesh_material"):
        try:
            body.set_editor_property(prop, mat)
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldCustom: {prop} skipped ({exc})")
    ocean.on_water_body_changed(shape_or_position_changed=False)
    world = ocean.get_world()
    if unreal.EditorAssetLibrary.does_asset_exist(CUSTOM_MAP):
        try:
            unreal.EditorAssetLibrary.delete_asset(CUSTOM_MAP)
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldCustom: stale map delete skipped ({exc})")
    if not unreal.EditorLoadingAndSavingUtils.save_map(world, CUSTOM_MAP):
        raise RuntimeError(f"failed to save_map {CUSTOM_MAP}")
    unreal.log("SeaShieldCustom: L_RangeCustom saved (MI_SeaOceanCustom ocean); L_Range untouched")


_state = {"phase": "build", "sec": 0.0, "h": None}


def _deferred(dt):
    _state["sec"] += dt
    try:
        if _state["phase"] == "build":
            _state["phase"] = "settle"
            _state["sec"] = 0.0
            main()
        elif _state["phase"] == "settle" and _state["sec"] >= 8.0:
            _state["phase"] = "done"
            unreal.log("SeaShieldCustom: DONE")
            unreal.unregister_slate_post_tick_callback(_state["h"])
            unreal.SystemLibrary.quit_editor()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldCustom: FAILED\n{traceback.format_exc()}")
        unreal.unregister_slate_post_tick_callback(_state["h"])
        unreal.SystemLibrary.quit_editor()


if __name__ == "__main__":
    _state["h"] = unreal.register_slate_post_tick_callback(_deferred)
