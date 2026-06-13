# One-shot patch for an existing L_Range (avoids the full setup_level.py
# regen + 30 s water settle): movable lights (kills the "LIGHTING NEEDS TO BE
# REBUILT" banner — fully dynamic Lumen path needs no baked data) and the
# water zone's far-distance skirt (horizon shows water past the gerstner
# zone, not a boundary band). setup_level.py carries the same settings for
# future regens. Run like setup_level.py:
#   UnrealEditor SeaShield.uproject -nullrhi -unattended \
#     -ExecCmds="py client/SeaShield/Tools/patch_level.py"
import unreal

LEVEL = "/Game/SeaShield/Maps/L_Range"


def main():
    level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if not level_subsystem.load_level(LEVEL):
        raise RuntimeError(f"failed to load {LEVEL}")

    by_label = {a.get_actor_label(): a for a in actor_subsystem.get_all_level_actors()}
    for label in ("Sun", "SkyLight"):
        actor = by_label.get(label)
        if actor is None:
            raise RuntimeError(f"missing actor {label}")
        actor.root_component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
        unreal.log(f"SeaShieldPatch: {label} -> movable")

    clouds = by_label.get("Clouds")
    if clouds is None:
        raise RuntimeError("missing actor Clouds")
    cloud_component = clouds.get_components_by_class(unreal.VolumetricCloudComponent)[0]
    # stat gpu: VolumetricCloud is the single biggest GPU item (6.2 of 17.4 ms
    # @1440p) and the view raymarch scales with this knob; 0.5 halves it for
    # no visible loss at the sim's camera distances.
    cloud_component.set_editor_property("view_sample_count_scale", 0.5)
    unreal.log("SeaShieldPatch: cloud view_sample_count_scale 0.5")

    zone = by_label.get("WaterZone")
    if zone is None:
        raise RuntimeError("missing actor WaterZone")
    far_material = unreal.load_asset("/Game/SeaShield/Materials/M_FarOcean")
    if far_material is None:
        raise RuntimeError("missing M_FarOcean (run setup_materials.py first)")
    water_mesh = zone.get_components_by_class(unreal.WaterMeshComponent)[0]
    water_mesh.set_editor_property("far_distance_material", far_material)
    water_mesh.set_editor_property("far_distance_mesh_extent", 4000000.0)
    unreal.log("SeaShieldPatch: far-distance skirt 40 km, M_FarOcean")


def _save():
    if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
        raise RuntimeError("failed to save level")
    unreal.log("SeaShieldPatch: saved")


_state = {"phase": "build", "seconds": 0.0, "handle": None}


def _deferred(delta_seconds):
    # Same defer/settle pattern as setup_level.py: -ExecCmds fires during
    # engine init, and the water mesh rebuild after the property edits is
    # asynchronous — give it a settle period before serializing.
    _state["seconds"] += delta_seconds
    try:
        if _state["phase"] == "build":
            _state["phase"] = "settle"
            _state["seconds"] = 0.0
            main()
        elif _state["phase"] == "settle" and _state["seconds"] >= 15.0:
            _state["phase"] = "done"
            _save()
            unreal.unregister_slate_post_tick_callback(_state["handle"])
            unreal.SystemLibrary.quit_editor()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldPatch: FAILED\n{traceback.format_exc()}")
        unreal.unregister_slate_post_tick_callback(_state["handle"])
        unreal.SystemLibrary.quit_editor()


if __name__ == "__main__":
    _state["handle"] = unreal.register_slate_post_tick_callback(_deferred)
