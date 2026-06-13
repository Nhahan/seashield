# Procedural materials for the procedural meshes — authored as node graphs in
# code (MaterialEditingLibrary), same discipline as the bpy generators. Run
# like setup_level.py (full editor, -ExecCmds="py ...").
import unreal

MAT_DIR = "/Game/SeaShield/Materials"
MESH_DIR = "/Game/SeaShield/Meshes"

_lib = unreal.MaterialEditingLibrary


def _new_material(name):
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = f"{MAT_DIR}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    material = tools.create_asset(name, MAT_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError(f"failed to create {path}")
    return material


def _const3(material, r, g, b, x=-600, y=0):
    node = _lib.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, x, y)
    node.set_editor_property("constant", unreal.LinearColor(r, g, b, 1.0))
    return node


def _const(material, value, x=-600, y=0):
    node = _lib.create_material_expression(material, unreal.MaterialExpressionConstant, x, y)
    node.set_editor_property("r", value)
    return node


def make_naval_hull():
    """Haze gray topsides, dark antifouling below the waterline (world Z 0),
    with a black boot-topping band — the classic warship paint scheme, driven
    purely by world height so it works on any hull the generators emit."""
    m = _new_material("M_NavalHull")
    haze = _const3(m, 0.085, 0.100, 0.110, -900, -200)
    antifoul = _const3(m, 0.095, 0.028, 0.022, -900, 0)
    boot = _const3(m, 0.020, 0.022, 0.025, -900, 200)

    world = _lib.create_material_expression(m, unreal.MaterialExpressionWorldPosition, -1200, 400)
    mask_z = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -1050, 400)
    mask_z.set_editor_property("r", False)
    mask_z.set_editor_property("g", False)
    mask_z.set_editor_property("b", True)
    _lib.connect_material_expressions(world, "", mask_z, "")

    # below = saturate((40 - z) / 30): 1 under the boot band, 0 above it.
    offset = _const(m, 40.0, -1050, 560)
    sub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -900, 480)
    _lib.connect_material_expressions(offset, "", sub, "A")
    _lib.connect_material_expressions(mask_z, "", sub, "B")
    scale = _const(m, 1.0 / 30.0, -900, 640)
    mul = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -750, 520)
    _lib.connect_material_expressions(sub, "", mul, "A")
    _lib.connect_material_expressions(scale, "", mul, "B")
    below = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -620, 520)
    _lib.connect_material_expressions(mul, "", below, "")

    # boot band = saturate((90 - z) / 25) - below: a stripe just above water.
    offset2 = _const(m, 90.0, -1050, 760)
    sub2 = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -900, 800)
    _lib.connect_material_expressions(offset2, "", sub2, "A")
    _lib.connect_material_expressions(mask_z, "", sub2, "B")
    scale2 = _const(m, 1.0 / 25.0, -900, 920)
    mul2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -750, 840)
    _lib.connect_material_expressions(sub2, "", mul2, "A")
    _lib.connect_material_expressions(scale2, "", mul2, "B")
    band = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -620, 840)
    _lib.connect_material_expressions(mul2, "", band, "")

    lerp1 = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -450, 0)
    _lib.connect_material_expressions(haze, "", lerp1, "A")
    _lib.connect_material_expressions(boot, "", lerp1, "B")
    _lib.connect_material_expressions(band, "", lerp1, "Alpha")
    lerp2 = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -300, 80)
    _lib.connect_material_expressions(lerp1, "", lerp2, "A")
    _lib.connect_material_expressions(antifoul, "", lerp2, "B")
    _lib.connect_material_expressions(below, "", lerp2, "Alpha")
    _lib.connect_material_property(lerp2, "", unreal.MaterialProperty.MP_BASE_COLOR)

    rough = _const(m, 0.52, -450, 300)
    _lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    metal = _const(m, 0.08, -450, 420)
    _lib.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def make_simple(name, rgb, roughness, metallic=0.1):
    m = _new_material(name)
    color = _const3(m, *rgb, -450, 0)
    _lib.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = _const(m, roughness, -450, 220)
    _lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    metal = _const(m, metallic, -450, 340)
    _lib.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def make_trail():
    """Unlit translucent ribbon smoke: tint and fade ride the vertex color
    alpha that SeaWorldManager writes per trail point (age-based)."""
    m = _new_material("M_RocketTrail")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided", True)

    vcolor = _lib.create_material_expression(m, unreal.MaterialExpressionVertexColor, -700, 0)
    tint = _const3(m, 0.88, 0.88, 0.90, -700, -180)
    color = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -480, -80)
    _lib.connect_material_expressions(tint, "", color, "A")
    _lib.connect_material_expressions(vcolor, "", color, "B")
    _lib.connect_material_property(color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    base_opacity = _const(m, 0.55, -700, 240)
    opacity = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -480, 200)
    _lib.connect_material_expressions(base_opacity, "", opacity, "A")
    _lib.connect_material_expressions(vcolor, "A", opacity, "B")
    _lib.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def make_splash():
    """Unlit translucent water column — bright aerated white with a slight
    cyan foot, vertical falloff so the top reads as spray."""
    m = _new_material("M_Splash")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided", True)

    body = _const3(m, 0.85, 0.93, 0.95, -700, -120)
    _lib.connect_material_property(body, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    opacity = _const(m, 0.62, -700, 160)
    _lib.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def make_rain():
    """Unlit translucent rain streaks: pale transmissive gray-blue, opacity
    driven by the vertex-color alpha SeaEnvironmentController writes per
    streak (distance falloff inside the camera volume)."""
    m = _new_material("M_Rain")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided", True)

    tint = _const3(m, 0.55, 0.62, 0.70, -700, -120)
    _lib.connect_material_property(tint, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    vcolor = _lib.create_material_expression(m, unreal.MaterialExpressionVertexColor, -700, 120)
    base_opacity = _const(m, 0.30, -700, 300)
    opacity = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -480, 200)
    _lib.connect_material_expressions(base_opacity, "", opacity, "A")
    _lib.connect_material_expressions(vcolor, "A", opacity, "B")
    _lib.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def make_far_ocean():
    """Opaque deep-sea sheet for the water zone's far-distance skirt — the
    flat ring beyond the 10.24 km gerstner zone. Color sits at the dark
    sea-surface average so the seam reads as distance, not a material edge;
    low roughness keeps the sun glint walking out to the horizon."""
    m = _new_material("M_FarOcean")
    color = _const3(m, 0.012, 0.032, 0.045, -450, 0)
    _lib.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = _const(m, 0.15, -450, 220)
    _lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    spec = _const(m, 0.25, -450, 340)
    _lib.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)
    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def make_burst():
    m = _new_material("M_Burst")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    body = _const3(m, 0.10, 0.095, 0.09, -700, -120)
    _lib.connect_material_property(body, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    opacity = _const(m, 0.72, -700, 160)
    _lib.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)
    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def assign(mesh_name, material):
    mesh = unreal.load_asset(f"{MESH_DIR}/{mesh_name}")
    if mesh is None:
        raise RuntimeError(f"missing mesh {mesh_name}")
    for slot in range(len(mesh.static_materials)):
        mesh.set_material(slot, material)
    unreal.EditorAssetLibrary.save_loaded_asset(mesh)


def main():
    hull = make_naval_hull()
    deck_gray = make_simple("M_NavalGray", (0.055, 0.062, 0.068), 0.6)
    missile_white = make_simple("M_MissileBody", (0.45, 0.45, 0.43), 0.38)
    rocket_olive = make_simple("M_RocketBody", (0.105, 0.115, 0.060), 0.55)
    make_trail()
    make_splash()
    make_burst()
    make_rain()
    make_far_ocean()

    assign("SM_Frigate", hull)
    assign("SM_LauncherBase", deck_gray)
    assign("SM_LauncherMount", deck_gray)
    assign("SM_LauncherTubes", deck_gray)
    assign("SM_Missile", missile_white)
    assign("SM_Rocket", rocket_olive)
    unreal.log("SeaShieldMaterials: 9 materials created, 6 meshes assigned")


def _deferred(_dt):
    unreal.unregister_slate_post_tick_callback(_HANDLE)
    try:
        main()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldMaterials: FAILED\n{traceback.format_exc()}")
    finally:
        unreal.SystemLibrary.quit_editor()


if __name__ == "__main__":
    _HANDLE = unreal.register_slate_post_tick_callback(_deferred)
