# Phase-7 LIGHTING probe. Naval-AD critic: ship is C/C+ mainly because the key light is
# flat/frontal — no terminator carving the faceted slabs. PROBED & CONFIRMED: the Sun is
# correctly at pitch -16, yaw 35 (not a position bug). Geometry: the hero cam looks NE from
# the SW and the sun's light travels toward az 35 (comes FROM ~az 215, SW) = behind the
# camera = FRONTAL = flat. This tests RAKING azimuths (light ~90 deg off the view) + a
# cloud-occlusion control, on throwaway save-as maps (NEVER writes L_Range).
# NB: on this UE build unreal.Rotator(a,b,c) maps to (roll=a, pitch=b, yaw=c) — verified by
# capture — so we build it as Rotator(0.0, pitch, yaw).
#   SEASHIELD_LIGHT=rakeSE UnrealEditor SeaShield.uproject -nullrhi -unattended \
#     -ExecCmds="py /abs/.../client/SeaShield/Tools/tune_light.py"
import os

import unreal

MAPS = "/Game/SeaShield/Maps"
# variant -> (map suffix, pitch, yaw, clouds_on). pitch NEGATIVE = above horizon.
VARIANTS = {
    "rakeSE":  ("L_Light_rakeSE", -22.0, 125.0, True),   # light from NW, rakes the SW-facing visible side
    "rakeNW":  ("L_Light_rakeNW", -22.0, 305.0, True),   # light from SE, rakes from the other side
    "nocloud": ("L_Light_nocloud", -16.0, 35.0, False),  # current angle, clouds HIDDEN (isolate occlusion)
}
VARIANT = os.environ.get("SEASHIELD_LIGHT", "rakeSE").strip().lower()


def main():
    key = next((k for k in VARIANTS if k.lower() == VARIANT), None)
    if key is None:
        raise RuntimeError(f"SEASHIELD_LIGHT={VARIANT!r} not in {list(VARIANTS)}")
    suffix, pitch, yaw, clouds_on = VARIANTS[key]
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    by_label = {a.get_actor_label(): a for a in actors.get_all_level_actors()}
    sun = by_label.get("Sun")
    if sun is None:
        raise RuntimeError("missing Sun actor")
    cur = sun.get_actor_rotation()
    unreal.log(f"SeaShieldLight: CURRENT Sun pitch={cur.pitch:.1f} yaw={cur.yaw:.1f} roll={cur.roll:.1f}")
    sun.set_actor_rotation(unreal.Rotator(0.0, pitch, yaw), False)  # (roll, pitch, yaw) on this build
    new = sun.get_actor_rotation()
    unreal.log(f"SeaShieldLight: SET {key} -> pitch={new.pitch:.1f} yaw={new.yaw:.1f} roll={new.roll:.1f}")
    clouds = by_label.get("Clouds")
    if clouds is not None and not clouds_on:
        clouds.set_actor_hidden_in_game(True)
        unreal.log("SeaShieldLight: Clouds HIDDEN (occlusion control)")
    world = sun.get_world()
    pkg = f"{MAPS}/{suffix}"
    if unreal.EditorAssetLibrary.does_asset_exist(pkg):
        try:
            unreal.EditorAssetLibrary.delete_asset(pkg)
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldLight: stale {pkg} delete skipped ({exc})")
    if not unreal.EditorLoadingAndSavingUtils.save_map(world, pkg):
        raise RuntimeError(f"failed to save_map {pkg}")
    unreal.log(f"SeaShieldLight: {pkg} saved; L_Range untouched")


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
            unreal.log("SeaShieldLight: DONE")
            unreal.unregister_slate_post_tick_callback(_state["h"])
            unreal.SystemLibrary.quit_editor()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldLight: FAILED\n{traceback.format_exc()}")
        unreal.unregister_slate_post_tick_callback(_state["h"])
        unreal.SystemLibrary.quit_editor()


if __name__ == "__main__":
    _state["h"] = unreal.register_slate_post_tick_callback(_deferred)
