# Focused water-material iteration: recreate the 3 ocean materials (M_FarOcean,
# MI_SeaOcean, MI_SeaOceanLOD) from setup_materials with their CURRENT values, then
# re-point the level's ocean body to them and re-save — one editor session, without
# the full 16-material setup_materials rebuild (and without dangling the ocean ref).
# For fast P3 water tuning (Phase 1 reflections/foam, Phase 4 color/atmosphere).
#   UnrealEditor SeaShield.uproject -nullrhi -unattended \
#     -ExecCmds="py /abs/.../client/SeaShield/Tools/tune_water.py"
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unreal

import setup_materials as sm

LEVEL = "/Game/SeaShield/Maps/L_Range"
MAT_DIR = "/Game/SeaShield/Materials"


def main():
    sm.make_far_ocean()
    sm.make_sea_ocean()
    unreal.log("SeaShieldTuneWater: ocean materials recreated")

    ls = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not ls.load_level(LEVEL):
        raise RuntimeError(f"failed to load {LEVEL}")
    by_label = {a.get_actor_label(): a for a in actors.get_all_level_actors()}

    zone = by_label.get("WaterZone")
    far = unreal.load_asset(f"{MAT_DIR}/M_FarOcean")
    if zone is not None and far is not None:
        wm = zone.get_components_by_class(unreal.WaterMeshComponent)[0]
        wm.set_editor_property("far_distance_material", far)

    ocean = by_label.get("Ocean")
    sea = unreal.load_asset(f"{MAT_DIR}/MI_SeaOcean")
    if ocean is None or sea is None:
        raise RuntimeError("missing Ocean actor or MI_SeaOcean")
    body = ocean.get_water_body_component()
    for prop in ("water_material", "water_static_mesh_material"):
        try:
            body.set_editor_property(prop, sea)
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldTuneWater: {prop} skipped ({exc})")
    ocean.on_water_body_changed(shape_or_position_changed=False)
    unreal.log("SeaShieldTuneWater: ocean re-pointed to MI_SeaOcean + M_FarOcean")


_state = {"phase": "build", "sec": 0.0, "h": None}


def _deferred(dt):
    _state["sec"] += dt
    try:
        if _state["phase"] == "build":
            _state["phase"] = "settle"
            _state["sec"] = 0.0
            main()
        elif _state["phase"] == "settle" and _state["sec"] >= 15.0:
            _state["phase"] = "done"
            if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
                raise RuntimeError("failed to save level")
            unreal.log("SeaShieldTuneWater: saved")
            unreal.unregister_slate_post_tick_callback(_state["h"])
            unreal.SystemLibrary.quit_editor()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldTuneWater: FAILED\n{traceback.format_exc()}")
        unreal.unregister_slate_post_tick_callback(_state["h"])
        unreal.SystemLibrary.quit_editor()


if __name__ == "__main__":
    _state["h"] = unreal.register_slate_post_tick_callback(_deferred)
