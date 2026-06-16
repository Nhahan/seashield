# Creates the base range level (L_Range) with the world manager placed — the
# code-authored starting point the editor work (sea, sky, weather visuals)
# builds on. Run like import_assets.py:
#   UnrealEditor SeaShield.uproject -nullrhi -unattended \
#     -ExecCmds="py client/SeaShield/Tools/setup_level.py"
# Idempotent: recreates the level from scratch each run.
import unreal

LEVEL = "/Game/SeaShield/Maps/L_Range"
# Must match SeaWorldFrame::Origin (Source/SeaShield/SeaWorldFrame.h).
STAGE_ORIGIN = (300000.0, 300000.0, 0.0)


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
    # Low golden-hour key (pitch -16, near the horizon) — long shadows, warm raking
    # form on the hull, and the SkyAtmosphere reddens the low sun for free; the
    # dimmed SkyLight gives the cool fill (cinematic key/fill, the biggest AA lever).
    sun = spawn(actor_subsystem, unreal.DirectionalLight, "Sun", rotation=(0.0, -16.0, 35.0))
    sun_comp = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sun_comp.set_editor_property("atmosphere_sun_light", True)
    sun_comp.set_editor_property("light_color", unreal.Color(255, 214, 165))  # warm golden key
    sun_comp.set_editor_property("light_source_angle", 1.1)  # softer shadow penumbra + sun disk
    # Screen-space contact shadows catch the small hull/launcher contact detail the
    # cascaded shadow map is too coarse for.
    sun_comp.set_editor_property("contact_shadow_length", 0.06)
    # Fully dynamic lighting (Lumen path): movable lights mean no baked data,
    # so the runtime never shows the "LIGHTING NEEDS TO BE REBUILT" banner.
    sun.root_component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    spawn(actor_subsystem, unreal.SkyAtmosphere, "SkyAtmosphere")
    sky_light = spawn(actor_subsystem, unreal.SkyLight, "SkyLight")
    sky_light.root_component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    skl = sky_light.get_component_by_class(unreal.SkyLightComponent)
    skl.set_editor_property("real_time_capture", True)
    # Dial the sky ambient FILL down so the warm sun casts real shadow contrast on
    # the hull (key/fill ratio = form). Auto-exposure lifts the overall back up, so
    # lit faces stay bright while shadows deepen — the "AA vs flat" difference.
    skl.set_editor_property("intensity", 0.62)
    # The real-time cubemap recapture re-renders sky+clouds per face — an ~18 ms
    # periodic spike at the default 128. 64 quarters the per-face cost; reflections
    # on the calm sea are too subtle to show the drop (performance-report §6.4).
    skl.set_editor_property("cubemap_resolution", 64)
    fog = spawn(actor_subsystem, unreal.ExponentialHeightFog, "HeightFog")
    _apply_height_fog(fog.get_component_by_class(unreal.ExponentialHeightFogComponent))
    clouds = spawn(actor_subsystem, unreal.VolumetricCloud, "Clouds")
    # Biggest GPU item by stat gpu (6.2 of 17.4 ms @1440p); 0.5 halves the
    # view raymarch with no visible loss at the sim's camera distances.
    clouds.get_components_by_class(unreal.VolumetricCloudComponent)[0].set_editor_property(
        "view_sample_count_scale", 0.5
    )
    # Open ocean sized for the engagement envelope (±20 km), with default
    # gerstner waves; the weather-driven wave/wind pass (K4) tunes this.
    # Zone sized to the close-action envelope; needs the matching
    # r.Water.WaterMesh.MaxWidthInTiles raise in DefaultEngine.ini or the
    # quadtree gets bias-capped and the surface breaks up. NOTE: do NOT put a
    # ground mesh under the ocean — the water info capture treats it as
    # terrain and snaps the zone's water surface down onto it (verified: the
    # surface hugged the seabed plane, jagged "shoreline" at the zone edge).
    # The play area lives at SeaWorldFrame::Origin = (3 km, 3 km): the Water
    # plugin renders a corrupted ~512 m patch anchored at WORLD ZERO on Metal
    # (probe-isolated: tracks neither camera, zone actor, spline nor meshes),
    # so the stage keeps its distance. Zone centered on the play area.
    water_zone = spawn(actor_subsystem, unreal.WaterZone, "WaterZone", location=STAGE_ORIGIN)
    water_zone.set_editor_property("zone_extent", unreal.Vector2D(1024000.0, 1024000.0))
    ocean = spawn(actor_subsystem, unreal.WaterBodyOcean, "Ocean")
    # Seeded gerstner spectrum via C++ (the waves classes are not scriptable).
    # 32 waves across a wide 3–80 m band kill the far-field repetition the
    # stock asset shows; seed/wind become weather-driven in the K4 pass.
    if not unreal.SeaLevelSetupLibrary.assign_generated_ocean_waves(
        ocean, 7, 32, 700.0, 9000.0, 3.0, 40.0, 40.0, 90.0, 0.42, 0.30
    ):
        raise RuntimeError("failed to assign ocean waves")
    # The root cause of the long-chased "dead 512 m square": a scripted ocean
    # keeps OceanExtents at its 51200 default instead of syncing to the zone.
    # This is the editor's own "Fill Water Zone With Ocean" button.
    component = ocean.get_water_body_component()
    component.call_method("FillWaterZoneWithOcean")
    # Far-distance skirt: a flat deep-sea ring extending 40 km past the
    # gerstner zone so the horizon shows water, not a zone-boundary band.
    water_mesh = water_zone.get_components_by_class(unreal.WaterMeshComponent)[0]
    water_mesh.set_editor_property(
        "far_distance_material", unreal.load_asset("/Game/SeaShield/Materials/M_FarOcean")
    )
    water_mesh.set_editor_property("far_distance_mesh_extent", 4000000.0)
    # Quality CEILING for the gerstner mesh. NOTE: the runtime frame budget biases
    # these DOWN via DefaultEngine.ini [SystemSettings] scalability CVars
    # (TessFactorBias/LODScaleBias/LODCountBias) — at factor 10 the depth prepass +
    # draw was 121 ms / 93% of the 1440p GPU frame (performance-report §6). The
    # distant-horizon anti-patterning is the MI_SeaOcean material's job (distant
    # normal + roughness), NOT the LOD density — so the budget bias is safe.
    for prop, value in (("tessellation_factor", 10), ("lod_scale", 1.5)):
        try:
            water_mesh.set_editor_property(prop, value)
        except Exception:  # noqa: BLE001
            pass
    # Calmer distant ocean: instances of the plugin's ocean materials that
    # flatten the far detail-normal and raise roughness so the 35 m distant
    # normal stops tiling into a horizon pattern and the near-mirror specular
    # stops aliasing into shimmer (setup_materials.make_sea_ocean). Waves/depth
    # color/foam are inherited from the plugin material.
    sea = unreal.load_asset("/Game/SeaShield/Materials/MI_SeaOcean")
    sea_lod = unreal.load_asset("/Game/SeaShield/Materials/MI_SeaOceanLOD")
    body = ocean.get_water_body_component()
    for prop, asset in (
        ("water_material", sea),
        ("water_lod_material", sea_lod),
        ("water_static_mesh_material", sea),
    ):
        try:
            body.set_editor_property(prop, asset)
        except Exception:  # noqa: BLE001
            pass
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
    # The pit at the origin, finally identified (r.Water.WaterInfo.
    # ShowSceneProxies=3 made it light up): the ocean's generated DILATED
    # water-info mesh — the one flagged for the info-texture depth-only pass —
    # leaks into the main depth pass on Metal and occludes the real surface.
    # Hide just that one in game: its only job is ground-depth dilation for
    # shoreline blending, and this map has no terrain. Hiding the non-dilated
    # info mesh too would blank the info texture and kill the detailed tiles.
    info_mesh_class = unreal.load_class(None, "/Script/Water.WaterBodyInfoMeshComponent")
    for info_component in ocean.get_components_by_class(info_mesh_class):
        if "Dilated" in info_component.get_name():
            info_component.set_editor_property("hidden_in_game", True)
    # Own-ship hull at the stage origin (the sim's ENU origin).
    frigate = spawn(actor_subsystem, unreal.StaticMeshActor, "Frigate", location=STAGE_ORIGIN)
    frigate.static_mesh_component.set_editor_property(
        "static_mesh", unreal.load_asset("/Game/SeaShield/Meshes/SM_Frigate")
    )

    # Unbound post-process grade: a gentle cinematic lift over the default
    # tonemap — modest bloom for the sky/sun glint, a touch more contrast and
    # saturation, and a soft vignette. No exposure override (keeps Lumen's
    # auto-exposure intact).
    ppv = spawn(actor_subsystem, unreal.PostProcessVolume, "PostProcess", location=STAGE_ORIGIN)
    apply_post_process(ppv)


def apply_post_process(ppv):
    ppv.set_editor_property("unbound", True)
    s = ppv.get_editor_property("settings")
    apply_grade(s)
    ppv.set_editor_property("settings", s)


def apply_grade(s):
    """Restrained cinematic grade ON TOP of the ACES filmic default (we shape, not
    replace, the tonemapper): glare bloom, a cool-shadow / warm-highlight wheel
    split, disciplined daylight exposure (narrow auto-exposure so panning sky->sea
    doesn't pump), and a soft vignette/fringe. Mirrored in patch_level.py."""
    scalars = {
        "bloom_intensity": 0.95,               # punchier glare so the new flash/airburst bloom
        "vignette_intensity": 0.36,
        "scene_fringe_intensity": 0.4,
        "auto_exposure_bias": -0.25,           # a touch richer, not murky
        "auto_exposure_min_brightness": 0.35,  # narrow range = stable daylight
        "auto_exposure_max_brightness": 2.0,
        "film_grain_intensity": 0.12,          # subtle cinematic tooth (kills the CG cleanliness)
    }
    for key, value in scalars.items():
        s.set_editor_property(f"override_{key}", True)
        s.set_editor_property(key, value)
    vectors = {
        "color_saturation": (1.13, 1.13, 1.15, 1.0),
        "color_contrast": (1.13, 1.13, 1.13, 1.0),
        # cool shadows + warm highlights (the grounded war-film split, restrained)
        "color_offset_shadows": (-0.012, -0.002, 0.016, 0.0),
        "color_gain_highlights": (1.07, 1.01, 0.92, 1.0),
    }
    for key, vec in vectors.items():
        s.set_editor_property(f"override_{key}", True)
        s.set_editor_property(key, unreal.Vector4(*vec))


def _apply_height_fog(fogc):
    """Aerial-perspective haze for distance depth: a thin, NON-volumetric height fog
    that builds toward the horizon (near water stays clear). UE5.7's luminance fog
    model leaves FogInscatteringLuminance at black and lets the SkyAtmosphere ambient
    (SkyAtmosphereAmbientContributionColorScale = white) drive the fog colour/
    brightness — physically-based, naturally cool-tinted at the horizon, no magic
    numbers. Cheap (no volumetric fog). Mirrored in patch_level.py."""
    fogc.set_editor_property("fog_density", 0.018)
    fogc.set_editor_property("fog_height_falloff", 0.12)
    fogc.set_editor_property("start_distance", 120000.0)  # cm (~1.2 km): keep near sea clear
    fogc.set_editor_property("fog_max_opacity", 0.85)     # don't fully white-out the horizon


def _save():
    if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level():
        raise RuntimeError("failed to save level")
    unreal.log(f"SeaShieldLevel: {LEVEL} created and saved")


_state = {"phase": "build", "seconds": 0.0, "handle": None}


def _deferred(delta_seconds):
    # -ExecCmds fires during engine init, where new_level + spawns crash;
    # defer one full editor tick so the world is stable. The save happens a
    # settle period AFTER construction: the water-info meshes build
    # asynchronously, and saving immediately serializes them half-baked.
    _state["seconds"] += delta_seconds
    try:
        if _state["phase"] == "build":
            _state["phase"] = "settle"
            _state["seconds"] = 0.0
            main()
        elif _state["phase"] == "settle" and _state["seconds"] >= 30.0:
            _state["phase"] = "done"
            _save()
            unreal.unregister_slate_post_tick_callback(_state["handle"])
            unreal.SystemLibrary.quit_editor()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldLevel: FAILED\n{traceback.format_exc()}")
        unreal.unregister_slate_post_tick_callback(_state["handle"])
        unreal.SystemLibrary.quit_editor()


if __name__ == "__main__":
    _state["handle"] = unreal.register_slate_post_tick_callback(_deferred)
