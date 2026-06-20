# Material-ONLY ocean rebuild: build + save M_Ocean / M_Wake / M_Spray in place, WITHOUT loading the
# L_Range(Custom) level. apply_ocean.py loads the Water-plugin map (the "Ocean" actor) to spawn
# SM_Ocean + save L_RangeCustom — and that live FWaterMeshSceneProxy races the material recompile's
# FlushRenderingCommands, firing the flaky FWaterQuadTree::~FWaterQuadTree() SIGSEGV (sometimes BEFORE
# the material saves). With no map loaded there is no Water proxy, so the recompile flush is safe.
# The throwaway L_RangeCustom already references these materials BY PATH, so re-saving the .uasset is
# enough for captures to pick up the rebuild (same trick as tune_mats.py for the ship materials).
# NEVER writes L_Range / L_RangeCustom. Real-RHI (no -nullrhi) so the translucent shader compiles.
#   UnrealEditor SeaShield.uproject -ExecCmds="py /abs/.../Tools/build_ocean_mat.py"
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unreal

import setup_materials as sm


def main():
    sm.make_ocean()   # the from-scratch ref-grounded translucent Gerstner ocean
    sm.make_wake()     # foam-ribbon material (wake / bow / hull collar)
    sm.make_spray()    # hull-waterline spray-puff ISM material
    # NOTE: do NOT rebuild M_FarOcean here — it carries the Water-plugin `used_with_water` usage, and
    # recompiling it triggers the WaterMesh SIGSEGV (same reason tune_mats.py avoids it). The horizon
    # 'bright band' lives in M_FarOcean (the far skirt beyond the Gerstner patch); fixing it must go
    # through apply_ocean (the Water-aware path), not this material-only build.
    unreal.log("SeaShieldOcean: material-only build DONE (M_Ocean + M_Wake + M_Spray)")


_state = {"phase": "build", "sec": 0.0, "h": None}


def _deferred(dt):
    _state["sec"] += dt
    try:
        if _state["phase"] == "build":
            _state["phase"] = "settle"
            _state["sec"] = 0.0
            main()
        elif _state["phase"] == "settle" and _state["sec"] >= 5.0:
            _state["phase"] = "done"
            unreal.log("SeaShieldOcean: DONE")
            unreal.unregister_slate_post_tick_callback(_state["h"])
            unreal.SystemLibrary.quit_editor()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldOcean: FAILED\n{traceback.format_exc()}")
        unreal.unregister_slate_post_tick_callback(_state["h"])
        unreal.SystemLibrary.quit_editor()


if __name__ == "__main__":
    _state["h"] = unreal.register_slate_post_tick_callback(_deferred)
