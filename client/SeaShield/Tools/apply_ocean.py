# From-scratch realistic ocean apply: build M_Ocean, spawn SM_Ocean at STAGE_ORIGIN with it, hide
# the Water-plugin Ocean surface render (buoyancy STILL samples its Gerstner waves -> determinism
# untouched), optionally add a PlanarReflection (probe-gated), save a THROWAWAY L_RangeCustom.
# NEVER writes L_Range. Real-RHI apply (no -nullrhi) so the translucent shader compiles.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unreal

import setup_materials as sm

STAGE_ORIGIN = unreal.Vector(300000.0, 300000.0, 0.0)
CUSTOM_MAP = "/Game/SeaShield/Maps/L_RangeCustom"
PLANAR = True  # translucent-forward validated; probe planar reflection (Metal stability)


def main():
    # neutralize any stale M_FarOcean debug emissive IN PLACE (never delete a live-referenced mat)
    far = unreal.load_asset("/Game/SeaShield/Materials/M_FarOcean")
    if far is not None:
        black = sm._const3(far, 0.0, 0.0, 0.0, -600, -400)
        unreal.MaterialEditingLibrary.connect_material_property(black, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        unreal.MaterialEditingLibrary.recompile_material(far)
        unreal.EditorAssetLibrary.save_loaded_asset(far)
    mat = sm.make_ocean()
    sm.make_wake()  # refresh the foam-ribbon material (wake/bow/hull-collar) with the organic breakup
    sm.make_spray()  # hull-waterline spray puff ISM material
    unreal.log("SeaShieldOcean: M_Ocean + M_Wake + M_Spray built")

    mesh = unreal.load_asset("/Game/SeaShield/Meshes/SM_Ocean")
    if mesh is None:
        raise RuntimeError("SM_Ocean missing — run import_ocean.py first")

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for a in list(actors.get_all_level_actors()):
        if a.get_actor_label() in ("Ocean_Cine", "CineOcean", "OceanReflect"):
            actors.destroy_actor(a)

    sma = actors.spawn_actor_from_object(mesh, STAGE_ORIGIN, unreal.Rotator(0.0, 0.0, 0.0))
    sma.set_actor_label("Ocean_Cine")
    sma.set_actor_scale3d(unreal.Vector(0.02, 0.02, 1.0))  # FBX ~100x -> ~1.2 km patch
    smc = sma.get_component_by_class(unreal.StaticMeshComponent)
    smc.set_mobility(unreal.ComponentMobility.MOVABLE)
    smc.set_material(0, mat)
    _b = sma.get_actor_bounds(False)
    unreal.log(f"SeaShieldOcean: Ocean_Cine spawned loc={sma.get_actor_location()} extent={_b[1]}")

    if PLANAR:
        try:
            pr_cls = getattr(unreal, "PlanarReflection", None)
            if pr_cls is not None:
                pr = actors.spawn_actor_from_class(pr_cls, STAGE_ORIGIN, unreal.Rotator(0.0, 0.0, 0.0))
                pr.set_actor_label("OceanReflect")
                pr.set_actor_scale3d(unreal.Vector(120.0, 120.0, 1.0))
                unreal.log("SeaShieldOcean: PlanarReflection spawned")
            else:
                unreal.log_warning("SeaShieldOcean: PlanarReflection class unavailable")
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldOcean: planar skipped ({exc})")

    # hide the plugin Ocean surface render (buoyancy still samples its waves)
    by_label = {a.get_actor_label(): a for a in actors.get_all_level_actors()}
    ocean = by_label.get("Ocean")
    if ocean is not None:
        ocean.set_actor_hidden_in_game(True)
        try:
            ocean.get_water_body_component().set_visibility(False, True)
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldOcean: body hide skipped ({exc})")
    unreal.log("SeaShieldOcean: plugin surface hidden")

    world = (ocean or sma).get_world()
    if unreal.EditorAssetLibrary.does_asset_exist(CUSTOM_MAP):
        unreal.EditorAssetLibrary.delete_asset(CUSTOM_MAP)
    if not unreal.EditorLoadingAndSavingUtils.save_map(world, CUSTOM_MAP):
        raise RuntimeError("save_map L_RangeCustom failed")
    unreal.log("SeaShieldOcean: L_RangeCustom saved (M_Ocean); L_Range untouched")


_state = {"phase": "build", "sec": 0.0, "h": None}


def _deferred(dt):
    _state["sec"] += dt
    try:
        if _state["phase"] == "build":
            _state["phase"] = "settle"
            _state["sec"] = 0.0
            main()
        elif _state["phase"] == "settle" and _state["sec"] >= 6.0:
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
