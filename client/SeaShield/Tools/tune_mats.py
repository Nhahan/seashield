# Focused Phase-7 ship-material iteration: rebuild ONLY M_NavalHull (waterline wetness) and
# M_SensorDark (black radar glass) in place, without the full 16-material setup_materials run.
# The mesh slots already reference these by path, so rebuilding the .uasset is enough. Mirrors
# the SensorDark args in setup_materials.main() (keep in sync). For fast naval-AD critic loops.
#   UnrealEditor SeaShield.uproject -nullrhi -unattended \
#     -ExecCmds="py /abs/.../client/SeaShield/Tools/tune_mats.py"
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unreal

import setup_materials as sm


def main():
    sm.make_naval_hull()  # picks up the widened waterline wetness band
    # black radar glass + gunmetal weapons — MUST match setup_materials.main() lines.
    sm.make_detailed("M_SensorDark", (0.020, 0.024, 0.032), 0.12, 0.6, tile_cm=150.0)
    sm.make_detailed("M_Gunmetal", (0.090, 0.100, 0.110), 0.30, 0.75, tile_cm=150.0)
    sm.make_far_ocean()   # P3-7.2: lifted far-ocean reflection value (water contrast)
    sm.make_sea_ocean()   # P3-7.4: grazing-roughness floor to calm the foreground mirror shards
    unreal.log("SeaShieldMats: M_NavalHull + M_SensorDark + M_Gunmetal + M_FarOcean + MI_SeaOcean rebuilt")


_state = {"phase": "build", "sec": 0.0, "h": None}


def _deferred(dt):
    _state["sec"] += dt
    try:
        if _state["phase"] == "build":
            _state["phase"] = "done"
            main()
            unreal.log("SeaShieldMats: DONE")
            unreal.unregister_slate_post_tick_callback(_state["h"])
            unreal.SystemLibrary.quit_editor()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldMats: FAILED\n{traceback.format_exc()}")
        unreal.unregister_slate_post_tick_callback(_state["h"])
        unreal.SystemLibrary.quit_editor()


if __name__ == "__main__":
    _state["h"] = unreal.register_slate_post_tick_callback(_deferred)
