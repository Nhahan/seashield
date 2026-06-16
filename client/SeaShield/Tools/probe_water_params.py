# One-off probe: dump the Water plugin ocean master material's parameter NAMES so
# make_sea_ocean can override the right ones (near-normal band / depth / SSS). The
# make_sea_ocean overrides currently only touch the DISTANT band — this finds the
# rest. Run like the other tools:
#   UnrealEditor SeaShield.uproject -nullrhi -unattended -nosplash \
#     -ExecCmds="py client/SeaShield/Tools/probe_water_params.py"
import unreal

_lib = unreal.MaterialEditingLibrary

TARGETS = [
    "/Water/Materials/WaterSurface/Water_Material_Ocean",
    "/Water/Materials/WaterSurface/LODs/Water_Material_Ocean_LOD",
]


def _names(getter, m):
    try:
        return [str(n) for n in getter(m)]
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(f"SeaShieldProbe: getter failed ({exc})")
        return []


def _dump(path):
    m = unreal.load_asset(path)
    if m is None:
        unreal.log_warning(f"SeaShieldProbe: MISSING {path}")
        return
    scalars = _names(_lib.get_scalar_parameter_names, m)
    vectors = _names(_lib.get_vector_parameter_names, m)
    textures = _names(_lib.get_texture_parameter_names, m)
    # Default getters on the MASTER material throw in 5.7; read inherited values via a
    # temporary Material Instance instead (the reliable path).
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    mi_path = "/Game/SeaShield/Materials/_ProbeMI"
    if unreal.EditorAssetLibrary.does_asset_exist(mi_path):
        unreal.EditorAssetLibrary.delete_asset(mi_path)
    mi = tools.create_asset("_ProbeMI", "/Game/SeaShield/Materials",
                            unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    mi.set_editor_property("parent", m)
    unreal.log(f"SeaShieldProbe: === {path} ===")
    for n in sorted(scalars):
        try:
            v = _lib.get_material_instance_scalar_parameter_value(mi, n)
        except Exception:  # noqa: BLE001
            v = "?"
        unreal.log(f"SeaShieldProbe: SCALAR | {n} = {v}")
    for n in sorted(vectors):
        try:
            c = _lib.get_material_instance_vector_parameter_value(mi, n)
            v = f"({c.r:.3f},{c.g:.3f},{c.b:.3f})"
        except Exception:  # noqa: BLE001
            v = "?"
        unreal.log(f"SeaShieldProbe: VECTOR | {n} = {v}")
    for n in sorted(textures):
        unreal.log(f"SeaShieldProbe: TEXTURE | {n}")
    unreal.EditorAssetLibrary.delete_asset(mi_path)
    unreal.log(f"SeaShieldProbe: {path} -> {len(scalars)} scalar, {len(vectors)} vector, {len(textures)} texture")


def main():
    for path in TARGETS:
        _dump(path)
    unreal.log("SeaShieldProbe: done")


def _deferred(_dt):
    unreal.unregister_slate_post_tick_callback(_HANDLE)
    try:
        main()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldProbe: FAILED\n{traceback.format_exc()}")
    finally:
        unreal.SystemLibrary.quit_editor()


if __name__ == "__main__":
    _HANDLE = unreal.register_slate_post_tick_callback(_deferred)
