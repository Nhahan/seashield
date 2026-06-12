# Creates the base range level (L_Range) with the world manager placed — the
# code-authored starting point the editor work (sea, sky, weather visuals)
# builds on. Run like import_assets.py:
#   UnrealEditor SeaShield.uproject -nullrhi -unattended \
#     -ExecCmds="py client/SeaShield/Tools/setup_level.py"
# Idempotent: recreates the level from scratch each run.
import unreal

LEVEL = "/Game/SeaShield/Maps/L_Range"


def spawn(actor_subsystem, cls, label, location=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0)):
    actor = actor_subsystem.spawn_actor_from_class(
        cls, unreal.Vector(*location), unreal.Rotator(*rotation)
    )
    if actor is None:
        raise RuntimeError(f"failed to spawn {label}")
    actor.set_actor_label(label)
    return actor


def main():
    level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)

    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL):
        # Re-run (it is also the editor startup map): step off it, drop it.
        level_subsystem.load_level("/Engine/Maps/Entry")
        unreal.EditorAssetLibrary.delete_asset(LEVEL)
    if not level_subsystem.new_level(LEVEL):
        raise RuntimeError(f"failed to create level {LEVEL}")

    manager_class = unreal.load_class(None, "/Script/SeaShield.SeaWorldManager")
    environment_class = unreal.load_class(None, "/Script/SeaShield.SeaEnvironmentController")
    spawn(actor_subsystem, manager_class, "SeaWorldManager")
    spawn(actor_subsystem, environment_class, "SeaEnvironmentController")

    # Minimal believable sea-sky lighting rig; the visual pass (K4) iterates
    # on these in the editor.
    # Rotator argument order is (roll, pitch, yaw): pitch -35 = afternoon sun.
    sun = spawn(actor_subsystem, unreal.DirectionalLight, "Sun", rotation=(0.0, -35.0, 40.0))
    sun.get_component_by_class(unreal.DirectionalLightComponent).set_editor_property(
        "atmosphere_sun_light", True
    )
    spawn(actor_subsystem, unreal.SkyAtmosphere, "SkyAtmosphere")
    spawn(actor_subsystem, unreal.SkyLight, "SkyLight").get_component_by_class(
        unreal.SkyLightComponent
    ).set_editor_property("real_time_capture", True)
    spawn(actor_subsystem, unreal.ExponentialHeightFog, "HeightFog")
    spawn(actor_subsystem, unreal.VolumetricCloud, "Clouds")
    # Open ocean sized for the engagement envelope (±20 km), with default
    # gerstner waves; the weather-driven wave/wind pass (K4) tunes this.
    # Zone sized to the close-action envelope; needs the matching
    # r.Water.WaterMesh.MaxWidthInTiles raise in DefaultEngine.ini or the
    # quadtree gets bias-capped and the surface breaks up. NOTE: do NOT put a
    # ground mesh under the ocean — the water info capture treats it as
    # terrain and snaps the zone's water surface down onto it (verified: the
    # surface hugged the seabed plane, jagged "shoreline" at the zone edge).
    water_zone = spawn(actor_subsystem, unreal.WaterZone, "WaterZone")
    water_zone.set_editor_property("zone_extent", unreal.Vector2D(819200.0, 819200.0))
    ocean = spawn(actor_subsystem, unreal.WaterBodyOcean, "Ocean")
    # The plugin's stock ocean waves asset — a bare new_object(
    # GerstnerWaterWaves) has no spectrum and collapses the detailed-mesh
    # surface to the floor (the "dry pit" we chased across several captures).
    # The wiring goes through C++ (USeaLevelSetupLibrary): the asset-reference
    # hop uses classes that are not exposed to scripting.
    if not unreal.SeaLevelSetupLibrary.assign_ocean_waves(
        ocean, "/Water/Waves/GerstnerWaves_Ocean"
    ):
        raise RuntimeError("failed to assign ocean waves")
    # The root cause of the long-chased "dead 512 m square": a scripted ocean
    # keeps OceanExtents at its 51200 default instead of syncing to the zone.
    # This is the editor's own "Fill Water Zone With Ocean" button.
    component = ocean.get_water_body_component()
    component.call_method("FillWaterZoneWithOcean")
    # The ocean spline outlines the ISLAND the ocean surrounds — its interior
    # plus the shape-dilation halo is punched dry (the long-chased pit at the
    # origin). Shrink both until the dry patch hides under the frigate hull.
    # CRITICAL: only actor.on_water_body_changed(shape_or_position_changed=
    # True) re-bakes the serialized water-info meshes; the spline's own
    # k2_synchronize broadcast does NOT (verified by capture pairs).
    ocean.get_water_body_component().set_editor_property("shape_dilation", 128.0)
    spline = ocean.get_water_spline()
    points = [
        spline.get_location_at_spline_point(i, unreal.SplineCoordinateSpace.LOCAL)
        for i in range(spline.get_number_of_spline_points())
    ]
    island_half_extent = max(max(abs(p.x), abs(p.y)) for p in points) or 1.0
    factor = 500.0 / island_half_extent
    for i, p in enumerate(points):
        spline.set_location_at_spline_point(
            i, unreal.Vector(p.x * factor, p.y * factor, p.z), unreal.SplineCoordinateSpace.LOCAL, True
        )
    ocean.on_water_body_changed(shape_or_position_changed=True)
    # Own-ship hull at the origin (the sim's launcher sits at ENU origin).
    frigate = spawn(actor_subsystem, unreal.StaticMeshActor, "Frigate")
    frigate.static_mesh_component.set_editor_property(
        "static_mesh", unreal.load_asset("/Game/SeaShield/Meshes/SM_Frigate")
    )

    if not level_subsystem.save_current_level():
        raise RuntimeError("failed to save level")
    unreal.log(f"SeaShieldLevel: {LEVEL} created and saved")


def _deferred(_delta_seconds):
    # -ExecCmds fires during engine init, where new_level + spawns crash;
    # defer one full editor tick so the world is stable.
    unreal.unregister_slate_post_tick_callback(_HANDLE)
    try:
        main()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldLevel: FAILED\n{traceback.format_exc()}")
    finally:
        unreal.SystemLibrary.quit_editor()


if __name__ == "__main__":
    _HANDLE = unreal.register_slate_post_tick_callback(_deferred)
