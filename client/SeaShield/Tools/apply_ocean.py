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
    # ROOT CAUSE OF THE MILKY HORIZON BAND (proven by a garish-green isolation test): it was M_Ocean's own
    # `_OCN_HAZE` grazing-merge — an emissive overlay baked into the NEAR patch — set to a pale (0.50,0.60,
    # 0.70). Tinting that constant green turned the band green; with it set deep blue-grey the band reads as
    # deep ocean. NOT the WaterZone WaterMesh, NOT the SkyAtmosphere, NOT the height fog (all ruled out:
    # red-skirt / atmosphere-hidden / fog-off still showed the band). The WaterMesh-hide + far-skirt + fog
    # re-tune below are RETAINED as insurance + a clearer mid-water look, but they were not the band's cause.
    # neutralize any stale M_FarOcean debug emissive IN PLACE (never delete/_new_material a live Water
    # material -> WaterMesh GetWaterMaterialRelevance SIGSEGV); just keeps the asset clean.
    far = unreal.load_asset("/Game/SeaShield/Materials/M_FarOcean")
    if far is not None:
        black = sm._const3(far, 0.0, 0.0, 0.0, -600, -400)
        unreal.MaterialEditingLibrary.connect_material_property(black, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        unreal.MaterialEditingLibrary.recompile_material(far)
        unreal.EditorAssetLibrary.save_loaded_asset(far)
    mat = sm.make_ocean()
    sm.make_wake()  # refresh the foam-ribbon material (wake/bow/hull-collar) with the organic breakup
    sm.make_spray()  # hull-waterline spray puff ISM material
    far_mat = sm.make_far_skirt()  # opaque/unlit far-distance sheet -> extends MY ocean to the horizon
    unreal.log("SeaShieldOcean: M_Ocean + M_Wake + M_Spray + M_OceanFar built")

    mesh = unreal.load_asset("/Game/SeaShield/Meshes/SM_Ocean")
    if mesh is None:
        raise RuntimeError("SM_Ocean missing — run import_ocean.py first")

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for a in list(actors.get_all_level_actors()):
        if a.get_actor_label() in ("Ocean_Cine", "Ocean_FarSkirt", "CineOcean", "OceanReflect"):
            actors.destroy_actor(a)

    sma = actors.spawn_actor_from_object(mesh, STAGE_ORIGIN, unreal.Rotator(0.0, 0.0, 0.0))
    sma.set_actor_label("Ocean_Cine")
    sma.set_actor_scale3d(unreal.Vector(0.02, 0.02, 1.0))  # FBX ~100x -> ~1.2 km patch
    smc = sma.get_component_by_class(unreal.StaticMeshComponent)
    smc.set_mobility(unreal.ComponentMobility.MOVABLE)
    smc.set_material(0, mat)
    _b = sma.get_actor_bounds(False)
    unreal.log(f"SeaShieldOcean: Ocean_Cine spawned loc={sma.get_actor_location()} extent={_b[1]}")

    # FAR-SKIRT: a 2nd SM_Ocean instance with the opaque M_OceanFar, scaled to reach PAST the horizon
    # and set 12 m BELOW the fine near patch so it (a) covers the hidden Water-plugin far tiles -> kills
    # the horizon band, (b) does NOT z-fight the near patch, and (c) the 12 m gap (>collar 9 m / >0 depth)
    # keeps it from falsely feeding the near patch's hull-collar depth-foam. The fine patch sits on top.
    far_loc = unreal.Vector(STAGE_ORIGIN.x, STAGE_ORIGIN.y, -1200.0)
    fsk = actors.spawn_actor_from_object(mesh, far_loc, unreal.Rotator(0.0, 0.0, 0.0))
    fsk.set_actor_label("Ocean_FarSkirt")
    fsk.set_actor_scale3d(unreal.Vector(0.55, 0.55, 1.0))  # ~33 km wide (16.5 km radius) -> edge well past the ~14 km horizon, closes the thin sky sliver
    fskc = fsk.get_component_by_class(unreal.StaticMeshComponent)
    fskc.set_mobility(unreal.ComponentMobility.MOVABLE)
    fskc.set_material(0, far_mat)
    unreal.log("SeaShieldOcean: Ocean_FarSkirt spawned (M_OceanFar, ~33km, z=-12m)")

    if PLANAR:
        try:
            pr_cls = getattr(unreal, "PlanarReflection", None)
            if pr_cls is not None:
                pr = actors.spawn_actor_from_class(pr_cls, STAGE_ORIGIN, unreal.Rotator(0.0, 0.0, 0.0))
                pr.set_actor_label("OceanReflect")
                pr.set_actor_scale3d(unreal.Vector(120.0, 120.0, 1.0))
                # NOTE: a tuned PlanarReflection + r.AllowGlobalClipPlane=1 was tested for a literal hull
                # mirror and did NOT take on this Metal forward-translucent water (capture-verified). Left as
                # a harmless minimal actor; the ship is anchored via the foam collar + waterline contact-
                # shadow (make_ocean) + the wake/spray riding the matched Gerstner surface instead.
                unreal.log("SeaShieldOcean: PlanarReflection spawned (minimal — forward water shows no hull mirror)")
            else:
                unreal.log_warning("SeaShieldOcean: PlanarReflection class unavailable")
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldOcean: planar skipped ({exc})")

    # hide the plugin Ocean surface render (buoyancy still samples its waves ANALYTICALLY in C++, so the
    # render can go) so the capture shows ONLY the from-scratch ocean. Hide both the "Ocean" WaterBody and
    # the WaterZone's WaterMeshComponent (the tile grid + far_distance mesh, far_distance_mesh_extent=40km,
    # setup_level.py) — the latter renders independently of the WaterBody. NOTE: this was once believed to
    # be the horizon-band culprit; the green-haze isolation test later proved the band was M_Ocean's own
    # _OCN_HAZE (see top of main). The hide is kept so the throwaway capture map shows MY ocean cleanly.
    by_label = {a.get_actor_label(): a for a in actors.get_all_level_actors()}
    ocean = by_label.get("Ocean")
    if ocean is not None:
        ocean.set_actor_hidden_in_game(True)
        try:
            ocean.get_water_body_component().set_visibility(False, True)
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldOcean: body hide skipped ({exc})")
    zone = by_label.get("WaterZone")
    if zone is not None:
        try:
            for wmc in zone.get_components_by_class(unreal.WaterMeshComponent):
                wmc.set_visibility(False, True)
                wmc.set_hidden_in_game(True, True)
            unreal.log("SeaShieldOcean: WaterZone WaterMesh hidden (capture shows only the from-scratch ocean)")
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldOcean: WaterZone mesh hide skipped ({exc})")
    else:
        unreal.log_warning("SeaShieldOcean: WaterZone actor not found — far band may persist")
    unreal.log("SeaShieldOcean: plugin surface hidden")

    # Height-fog re-tune — a QUALITY pass, NOT the band fix (the band was M_Ocean's _OCN_HAZE, see top of
    # main). setup_level.py's ExponentialHeightFog ("HeightFog") starts at just 720 m and caps at 0.85
    # opacity, which over-hazes the mid water. Re-tune HERE (saved into throwaway L_RangeCustom only; the
    # gameplay L_Range fog is untouched): push the haze far out so near/mid water stays crystal-clear, drop
    # max-opacity, and cool the sky-ambient tint to a dim blue so what haze remains reads as ocean-distance
    # aerial perspective rather than a pale wash.
    fog = by_label.get("HeightFog")
    if fog is not None:
        try:
            fc = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
            # NOTE: density + start_distance are OVERRIDDEN at runtime by ASeaEnvironmentController from
            # the seed weather (humidity/rain). max_opacity + height_falloff have NO runtime setter, so the
            # CEILING/shape is set here: a high max_opacity UNCAPS the fog so humid/rain seeds build real
            # murk (clear seeds stay clear — density drives the amount); a higher falloff makes the fog
            # low-lying (sea-mist 해무 hugging the water) rather than a tall column.
            fc.set_editor_property("start_distance", 220000.0)   # initial; runtime-overridden per weather
            fc.set_editor_property("fog_density", 0.018)         # initial; runtime-overridden per weather
            fc.set_editor_property("fog_max_opacity", 0.92)      # UNCAP (was 0.25) -> dramatic weather murk
            fc.set_editor_property("fog_height_falloff", 0.30)   # low-lying sea fog (was 0.12, taller column)
            # THE colour lever: the fog was tinted by the WHITE SkyAtmosphere ambient -> a MILKY band.
            # Dim+cool that contribution to a deep blue so the aerial haze reads as ocean-distance haze
            # (physically bluish), not a grey wall. wb10 proved opacity alone can't fix the *colour*.
            fc.set_editor_property("sky_atmosphere_ambient_contribution_color_scale",
                                   unreal.LinearColor(0.14, 0.22, 0.34, 1.0))
            # UE5.8 light-scattering (so the seed-random weather fog/mist interacts with the sun/sky):
            # Fog Screen-Space Scattering + sky-light-capture inscattering. NOTE: density/start here are
            # only an initial look — ASeaEnvironmentController overrides them at RUNTIME from the weather
            # humidity/rain, so each captured seed draws a different sky.
            for _p, _v in (("enable_fsss", True),
                           ("sky_light_capture_affects_height_fog_strength", 0.5),
                           ("sky_light_capture_affects_height_fog_roughness", 0.5)):
                try:
                    fc.set_editor_property(_p, _v)
                except Exception:  # noqa: BLE001
                    pass
            # UE5.8 VOLUMETRIC FOG — the sea mist (해무) as REAL 3D god-ray scattering, not a flat
            # screen haze: the low sun scatters THROUGH the marine mist in froxels, and the sea-mist
            # Local Fog Volume voxelizes into this (r.LocalFogVolume.RenderIntoVolumetricFog=1 default)
            # so it FINALLY renders AND applies over the translucent ocean — the earlier "Metal can't
            # render LFV" was a misdiagnosis (the translucent ocean was overdrawing the LFV tiled pass;
            # volumetric fog is sampled per-pixel including on translucency). enable_volumetric_fog has
            # NO runtime setter, so it is STATIC here; the AMOUNT follows the runtime weather density
            # (volumetric fog = fog_density × extinction_scale), and ASeaEnvironmentController drives
            # the extinction/phase from the seed weather. Volumetric fog ignores StartDistance/MaxOpacity
            # in its range, so the analytic FAR veil (runtime StartDistance) still owns the far horizon —
            # volumetric = near/mid god-ray mist, analytic = cheap distance haze (they composite).
            for _p, _v in (("enable_volumetric_fog", True),
                           ("volumetric_fog_distance", 20000.0),               # 200 m god-ray range covers the hero ship
                           ("volumetric_fog_scattering_distribution", 0.30),   # visible side shafts + a forward sun glow
                           ("volumetric_fog_extinction_scale", 1.5),           # punchier mist (weather density still drives the amount)
                           ("volumetric_fog_albedo", unreal.Color(r=235, g=240, b=245, a=255))):  # near-white water particles
                try:
                    fc.set_editor_property(_p, _v)
                except Exception:  # noqa: BLE001
                    pass
            unreal.log("SeaShieldOcean: HeightFog re-tuned (maxop 0.92, ambient->dim-blue, FSSS + volumetric god-ray mist on)")
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldOcean: HeightFog re-tune skipped ({exc})")
    else:
        unreal.log_warning("SeaShieldOcean: HeightFog actor not found — milky band may persist")

    # ---- DIAGNOSTIC ISOLATION (SEA_DIAG_BAND=1) ----------------------------------------------------
    # The horizon band is byte-invariant to height-fog colour/opacity AND to far-skirt size (wb9/10/11).
    # Prime suspect: the SkyAtmosphere's AERIAL PERSPECTIVE (atmospheric scattering applied to distant
    # geometry), a SEPARATE lever from ExponentialHeightFog. To isolate, paint the far-skirt PURE RED,
    # HIDE the SkyAtmosphere, and kill the height fog. Read the one capture:
    #   band RED            -> the band is the SEA (far-skirt) and the atmosphere wash was the tint
    #   band GONE/very dark -> the band was the SkyAtmosphere aerial perspective (tune THAT, not fog)
    #   band still pale     -> something else entirely (skybox / cubemap) is painting the horizon
    if os.environ.get("SEA_DIAG_BAND"):
        try:
            red = sm._new_material("M_DiagRed")
            red.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
            red.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
            unreal.MaterialEditingLibrary.connect_material_property(
                sm._const3(red, 1.0, 0.0, 0.0, -300, 0), "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
            unreal.MaterialEditingLibrary.recompile_material(red)
            fskc.set_material(0, red)
            atm = by_label.get("SkyAtmosphere")
            if atm is not None:
                atm.set_actor_hidden_in_game(True)
                for comp in atm.get_components_by_class(unreal.SkyAtmosphereComponent):
                    comp.set_visibility(False, True)
            if fog is not None:
                fog.get_component_by_class(unreal.ExponentialHeightFogComponent).set_editor_property("fog_max_opacity", 0.0)
            unreal.log("SeaShieldOcean: DIAG — far-skirt RED, SkyAtmosphere hidden, fog off")
        except Exception as exc:  # noqa: BLE001
            unreal.log_warning(f"SeaShieldOcean: DIAG block failed ({exc})")
    # -----------------------------------------------------------------------------------------------

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
