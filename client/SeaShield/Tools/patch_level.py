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


def _apply_grade(s):
    """Mirror of setup_level.apply_grade — restrained cinematic grade over the ACES
    filmic default: glare bloom, cool-shadow / warm-highlight wheels, disciplined
    daylight exposure, soft vignette/fringe."""
    scalars = {
        "bloom_intensity": 0.95,
        "vignette_intensity": 0.36,
        "scene_fringe_intensity": 0.4,
        "auto_exposure_bias": -0.25,
        "auto_exposure_min_brightness": 0.35,
        "auto_exposure_max_brightness": 2.0,
        "film_grain_intensity": 0.12,
    }
    for key, value in scalars.items():
        s.set_editor_property(f"override_{key}", True)
        s.set_editor_property(key, value)
    vectors = {
        "color_saturation": (1.13, 1.13, 1.15, 1.0),
        "color_contrast": (1.13, 1.13, 1.13, 1.0),
        "color_offset_shadows": (-0.012, -0.002, 0.016, 0.0),
        "color_gain_highlights": (1.07, 1.01, 0.92, 1.0),
    }
    for key, vec in vectors.items():
        s.set_editor_property(f"override_{key}", True)
        s.set_editor_property(key, unreal.Vector4(*vec))


def _apply_height_fog(fogc):
    """Mirror of setup_level._apply_height_fog — thin sky-driven aerial-perspective
    haze (UE5.7 luminance model: SkyAtmosphere ambient drives the fog colour)."""
    fogc.set_editor_property("fog_density", 0.018)
    fogc.set_editor_property("fog_height_falloff", 0.12)
    fogc.set_editor_property("start_distance", 120000.0)
    fogc.set_editor_property("fog_max_opacity", 0.85)


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

    # Lighting: low raking golden key (pitch -24) for form on the hull, warm key
    # color, contact shadows, cheaper real-time sky cubemap (128->64 kills the
    # ~18 ms recapture spike; performance-report §6.4).
    sun = by_label["Sun"]
    sun.set_actor_rotation(unreal.Rotator(0.0, -16.0, 35.0), False)
    sun_comp = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sun_comp.set_editor_property("light_color", unreal.Color(255, 214, 165))
    sun_comp.set_editor_property("light_source_angle", 1.1)
    sun_comp.set_editor_property("contact_shadow_length", 0.06)
    skl = by_label["SkyLight"].get_component_by_class(unreal.SkyLightComponent)
    skl.set_editor_property("cubemap_resolution", 64)
    skl.set_editor_property("intensity", 0.62)  # lower sky fill -> shadow contrast/form
    unreal.log("SeaShieldPatch: golden key + low sky fill + contact shadows + cubemap 64")

    fog = by_label.get("HeightFog")
    if fog is not None:
        _apply_height_fog(fog.get_component_by_class(unreal.ExponentialHeightFogComponent))
        unreal.log("SeaShieldPatch: aerial-perspective height fog")

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

    # Quality CEILING for the gerstner mesh — the runtime frame budget biases these
    # DOWN via DefaultEngine.ini [SystemSettings] (TessFactorBias/LODScaleBias/
    # LODCountBias): factor 10 made water 121 ms / 93% of the 1440p GPU frame
    # (performance-report §6). Distant anti-patterning is the MI_SeaOcean material's
    # job, not the LOD, so the bias is safe.
    for prop, value in (("tessellation_factor", 10), ("lod_scale", 1.5)):
        try:
            water_mesh.set_editor_property(prop, value)
            unreal.log(f"SeaShieldPatch: water mesh {prop} -> {value}")
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldPatch: water mesh {prop} skipped ({exc})")

    # Calmer distant ocean: swap the stock ocean materials for our instances that
    # flatten the far detail-normal and raise roughness (kills the horizon
    # tiling/shimmer). Inherit everything else from the plugin material.
    ocean = by_label.get("Ocean")
    if ocean is None:
        raise RuntimeError("missing actor Ocean")
    sea = unreal.load_asset("/Game/SeaShield/Materials/MI_SeaOcean")
    sea_lod = unreal.load_asset("/Game/SeaShield/Materials/MI_SeaOceanLOD")
    if sea is None or sea_lod is None:
        raise RuntimeError("missing MI_SeaOcean (run setup_materials.py first)")
    body = ocean.get_water_body_component()
    for prop, asset in (
        ("water_material", sea),
        ("water_lod_material", sea_lod),
        ("water_static_mesh_material", sea),
    ):
        try:
            body.set_editor_property(prop, asset)
            unreal.log(f"SeaShieldPatch: ocean {prop} set")
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldPatch: ocean {prop} skipped ({exc})")
    # Rebuild the body's material instances + the water mesh's gathered materials.
    ocean.on_water_body_changed(shape_or_position_changed=False)

    # Unbound post-process grade: a gentle cinematic lift over the default
    # tonemap (modest bloom, slight contrast/saturation, soft vignette). No
    # exposure override so Lumen auto-exposure is untouched.
    ppv = by_label.get("PostProcess")
    if ppv is None:
        ppv = actor_subsystem.spawn_actor_from_class(
            unreal.PostProcessVolume, unreal.Vector(0.0, 0.0, 0.0), unreal.Rotator()
        )
        ppv.set_actor_label("PostProcess")
    ppv.set_editor_property("unbound", True)
    settings = ppv.get_editor_property("settings")
    _apply_grade(settings)
    ppv.set_editor_property("settings", settings)
    unreal.log("SeaShieldPatch: post-process grade applied")


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
