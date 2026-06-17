# Headless import of the procedural PBR detail maps (tools/assets/out/textures)
# into the UE project as Texture2D, with the right compression/color-space for a
# data map (NOT sRGB) and a normal map. Run via the full editor:
#   UnrealEditor SeaShield.uproject -nullrhi -unattended \
#     -ExecCmds="py client/SeaShield/Tools/import_textures.py"
# Idempotent (replace_existing). Materials sample these triplanar (setup_materials.py).
import os

import unreal

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
SRC = os.path.join(REPO_ROOT, "tools", "assets", "out", "textures")
DEST = "/Game/SeaShield/Textures"

# (file, asset name, kind): normal | masks | sprite
TEXTURES = [
    ("T_ShipDetail_N.png", "T_ShipDetail_N", "normal"),
    ("T_ShipDetail_RAO.png", "T_ShipDetail_RAO", "masks"),
    # Second baked detail variation — blended against the first by a macro mask in the
    # material to break the triplanar tile repeat on the hull (anti-tiling, P2-5).
    ("T_ShipDetail_N2.png", "T_ShipDetail_N2", "normal"),
    ("T_ShipDetail_RAO2.png", "T_ShipDetail_RAO2", "masks"),
    # Ocean micro-surface normal (TRACK W) — sampled at 3 world scales in M_SeaOceanCustom
    # to give the DEFAULT_LIT sea the high-frequency facets it needs for sun/sky glitter.
    ("T_WaterRipple_N.png", "T_WaterRipple_N", "normal"),
    # Hull-markings decal stencil (pennant number/draft marks/hatches) — world-projected once
    # onto the bow topsides in M_NavalHull for human-scale reference (data map, not sRGB).
    ("T_HullMarkings.png", "T_HullMarkings", "masks"),
    # Deck-markings decal (helo circle/non-skid/hatches) — top-down world-projected once.
    ("T_DeckMarkings.png", "T_DeckMarkings", "masks"),
    ("T_Smoke.png", "T_Smoke", "sprite"),
    ("T_Flash.png", "T_Flash", "sprite"),
]


def import_texture(path, name, kind):
    task = unreal.AssetImportTask()
    task.filename = path
    task.destination_path = DEST
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = True
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    tex = unreal.load_asset(f"{DEST}/{name}")
    if tex is None:
        raise RuntimeError(f"texture import failed: {path}")
    if kind == "normal":
        tex.set_editor_property("compression_settings",
                                unreal.TextureCompressionSettings.TC_NORMALMAP)
        tex.set_editor_property("srgb", False)
    elif kind == "masks":
        # Packed data (roughness/AO/dirt) — linear, per-channel, never sRGB.
        tex.set_editor_property("compression_settings",
                                unreal.TextureCompressionSettings.TC_MASKS)
        tex.set_editor_property("srgb", False)
    else:  # RGBA colour sprite (smoke/flash billboard) — keep alpha, sRGB colour.
        tex.set_editor_property("compression_settings",
                                unreal.TextureCompressionSettings.TC_DEFAULT)
        tex.set_editor_property("srgb", True)
    if name in ("T_HullMarkings", "T_DeckMarkings"):
        # CLAMP addressing so the world-projected decal appears ONCE (black border outside [0,1]),
        # not tiled across the hull/deck.
        tex.set_editor_property("address_x", unreal.TextureAddress.TA_CLAMP)
        tex.set_editor_property("address_y", unreal.TextureAddress.TA_CLAMP)
    unreal.EditorAssetLibrary.save_loaded_asset(tex)
    unreal.log(f"SeaShieldTextures: {name} ({kind}) imported")
    return tex


def main():
    for filename, name, is_normal in TEXTURES:
        path = os.path.join(SRC, filename)
        if not os.path.isfile(path):
            raise RuntimeError(f"missing PNG (run tools/assets/textures.py first): {path}")
        import_texture(path, name, is_normal)
    unreal.log(f"SeaShieldTextures: imported {len(TEXTURES)} detail maps")


if __name__ == "__main__":
    try:
        main()
    except Exception:  # noqa: BLE001
        import traceback

        unreal.log_error(f"SeaShieldTextures: FAILED\n{traceback.format_exc()}")
    finally:
        unreal.SystemLibrary.quit_editor()
