# Procedural materials for the procedural meshes — authored as node graphs in
# code (MaterialEditingLibrary), same discipline as the bpy generators. Run
# like setup_level.py (full editor, -ExecCmds="py ...").
import math
import re

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


def _panned_noise(m, scale, pan, cut, gain, ax=-560, ay=560):
    """World-space, time-panned turbulence eroded to a contrasted 0..1 mask — the
    wispy-edge node factored out of make_trail (the verified Phase-2 smoke) so the
    other soft VFX (splash/burst/muzzle/wake) read as broken, turbulent volume
    instead of solid primitives. `scale` ~ inverse feature size (0.0025 ≈ 4 m);
    `pan` is the per-axis world drift speed; `cut`/`gain` bite the contrast."""
    worldpos = _lib.create_material_expression(m, unreal.MaterialExpressionWorldPosition, ax - 460, ay - 40)
    timev = _lib.create_material_expression(m, unreal.MaterialExpressionTime, ax - 460, ay + 120)
    panvec = _const3(m, pan[0], pan[1], pan[2], ax - 460, ay + 240)
    panmul = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax - 300, ay + 140)
    _lib.connect_material_expressions(timev, "", panmul, "A")
    _lib.connect_material_expressions(panvec, "", panmul, "B")
    npos = _lib.create_material_expression(m, unreal.MaterialExpressionAdd, ax - 150, ay)
    _lib.connect_material_expressions(worldpos, "", npos, "A")
    _lib.connect_material_expressions(panmul, "", npos, "B")
    noise = _lib.create_material_expression(m, unreal.MaterialExpressionNoise, ax, ay)
    noise.set_editor_property("scale", scale)
    noise.set_editor_property("levels", 2)
    noise.set_editor_property("output_min", 0.0)
    noise.set_editor_property("output_max", 1.0)
    noise.set_editor_property("turbulence", True)
    _lib.connect_material_expressions(npos, "", noise, "Position")
    nsub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, ax + 170, ay)
    ncut = _const(m, cut, ax, ay + 170)
    _lib.connect_material_expressions(noise, "", nsub, "A")
    _lib.connect_material_expressions(ncut, "", nsub, "B")
    nmul = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 320, ay)
    ngain = _const(m, gain, ax + 170, ay + 170)
    _lib.connect_material_expressions(nsub, "", nmul, "A")
    _lib.connect_material_expressions(ngain, "", nmul, "B")
    nsat = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, ax + 470, ay)
    _lib.connect_material_expressions(nmul, "", nsat, "")
    return nsat


def _fresnel(m, exponent, ax=-600, ay=300):
    """Fresnel term (≈0 facing, ≈1 grazing). Used to feather the rounded VFX
    silhouettes into transparency (multiply by 1-Fresnel) and to blend a hot core
    to a cool edge on the airburst — what turns a solid sphere into a soft puff."""
    f = _lib.create_material_expression(m, unreal.MaterialExpressionFresnel, ax, ay)
    f.set_editor_property("exponent", exponent)
    return f


_TEX_DIR = "/Game/SeaShield/Textures"
_TEX_CACHE = {}


def _detail_tex(name):
    if name not in _TEX_CACHE:
        t = unreal.load_asset(f"{_TEX_DIR}/{name}")
        if t is None:
            raise RuntimeError(f"missing detail texture {name} — run import_textures.py first")
        _TEX_CACHE[name] = t
    return _TEX_CACHE[name]


def _triplanar(m, texture, world_size_cm, sampler_type, ax, ay):
    """World-space TRIPLANAR sample of `texture` — no UVs needed (the procedural
    meshes have none). Projects WorldPosition onto the XY/YZ/XZ planes, samples
    each, and blends by |vertex normal|. Returns the blended sample node (RGB).
    `world_size_cm` is the world span of one texture tile; `sampler_type` is
    SAMPLERTYPE_NORMAL (detail normal) or SAMPLERTYPE_MASKS (packed R/AO/dirt)."""
    wp = _lib.create_material_expression(m, unreal.MaterialExpressionWorldPosition, ax - 760, ay)
    invs = _const(m, 1.0 / world_size_cm, ax - 760, ay + 170)
    pos = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax - 620, ay)
    _lib.connect_material_expressions(wp, "", pos, "A")
    _lib.connect_material_expressions(invs, "", pos, "B")

    def mask(node, r, g, b, yy):
        mk = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, ax - 470, yy)
        mk.set_editor_property("r", r)
        mk.set_editor_property("g", g)
        mk.set_editor_property("b", b)
        mk.set_editor_property("a", False)
        _lib.connect_material_expressions(node, "", mk, "")
        return mk

    uv_xy = mask(pos, True, True, False, ay - 140)   # facing +Z (deck)
    uv_yz = mask(pos, False, True, True, ay)         # facing +X (port/stbd sides)
    uv_xz = mask(pos, True, False, True, ay + 140)   # facing +Y (bow/stern)

    def samp(uv, yy):
        ts = _lib.create_material_expression(m, unreal.MaterialExpressionTextureSample, ax - 300, yy)
        ts.set_editor_property("texture", texture)
        ts.set_editor_property("sampler_type", sampler_type)
        _lib.connect_material_expressions(uv, "", ts, "UVs")
        return ts

    s_z = samp(uv_xy, ay - 140)
    s_x = samp(uv_yz, ay)
    s_y = samp(uv_xz, ay + 140)

    vn = _lib.create_material_expression(m, unreal.MaterialExpressionVertexNormalWS, ax - 470, ay + 320)
    an = _lib.create_material_expression(m, unreal.MaterialExpressionAbs, ax - 330, ay + 320)
    _lib.connect_material_expressions(vn, "", an, "")
    wx = mask(an, True, False, False, ay + 300)
    wy = mask(an, False, True, False, ay + 360)
    wz = mask(an, False, False, True, ay + 420)

    def mul(a, b, yy):
        n = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax - 120, yy)
        _lib.connect_material_expressions(a, "", n, "A")
        _lib.connect_material_expressions(b, "", n, "B")
        return n

    def add(a, b, xx, yy):
        n = _lib.create_material_expression(m, unreal.MaterialExpressionAdd, xx, yy)
        _lib.connect_material_expressions(a, "", n, "A")
        _lib.connect_material_expressions(b, "", n, "B")
        return n

    px = mul(s_x, wx, ay)
    py = mul(s_y, wy, ay + 60)
    pz = mul(s_z, wz, ay + 120)
    num = add(add(px, py, ax + 40, ay + 20), pz, ax + 180, ay + 60)
    den = add(add(wx, wy, ax + 40, ay + 320), wz, ax + 180, ay + 360)
    res = _lib.create_material_expression(m, unreal.MaterialExpressionDivide, ax + 320, ay)
    _lib.connect_material_expressions(num, "", res, "A")
    _lib.connect_material_expressions(den, "", res, "B")
    return res


def _macro_variation(m, ax, ay, scale=0.00035):
    """A STATIC, large-scale (~30 m) world-space noise field, 0..1 — the anti-tiling
    macro mask (P2-5). Reuses _panned_noise with ZERO pan (static) at a low scale (big
    features). Used to (a) blend two baked detail variations on the hull and (b) modulate
    per-region weathering elsewhere, so the tiling detail stops reading as a uniform repeat."""
    return _panned_noise(m, scale, (0.0, 0.0, 0.0), 0.0, 1.0, ax=ax, ay=ay)


def _detail_normal(m, world_size_cm, ax, ay, tex="T_ShipDetail_N"):
    """Triplanar detail normal (panel/plate/weld relief) -> normalized tangent
    normal for MP_NORMAL."""
    blended = _triplanar(m, _detail_tex(tex), world_size_cm,
                         unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, ax, ay)
    nrm = _lib.create_material_expression(m, unreal.MaterialExpressionNormalize, ax + 460, ay)
    _lib.connect_material_expressions(blended, "", nrm, "")
    return nrm


def _detail_normal_db(m, coarse_cm, fine_cm, near_cm, far_cm, ax, ay, tex="T_ShipDetail_N",
                      tex2=None, macro=None):
    """Distance-blended triplanar detail normal: the COARSE plate/weld relief always,
    PLUS a finer sub-panel/rivet tile faded IN as the camera approaches (PixelDepth)
    and OUT in the distance — so close-ups gain crisp micro-detail while far hull
    stays shimmer-free (keeps the §6.4/6.7 anti-shimmer discipline). The fine term is
    capped so the coarse relief always dominates; lerp+normalize is the standard cheap
    detail-normal blend. ANTI-TILING (P2-5): if tex2+macro given, the COARSE band is
    macro-blended between two baked variations (different plate layouts per region) so
    the plate pattern stops repeating; the fine band stays single-variation (cost)."""
    ntex = _detail_tex(tex)
    coarse = _triplanar(m, ntex, coarse_cm, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, ax, ay)
    if tex2 is not None and macro is not None:
        coarse2 = _triplanar(m, _detail_tex(tex2), coarse_cm,
                             unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, ax, ay + 2350)
        cmix = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, ax + 250, ay)
        _lib.connect_material_expressions(coarse, "", cmix, "A")
        _lib.connect_material_expressions(coarse2, "", cmix, "B")
        _lib.connect_material_expressions(macro, "", cmix, "Alpha")
        coarse = cmix
    fine = _triplanar(m, ntex, fine_cm, unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL, ax, ay + 1100)
    depth = _lib.create_material_expression(m, unreal.MaterialExpressionPixelDepth, ax - 260, ay + 1900)
    fr = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, ax - 100, ay + 1900)
    farc = _const(m, float(far_cm), ax - 260, ay + 2040)
    _lib.connect_material_expressions(farc, "", fr, "A")
    _lib.connect_material_expressions(depth, "", fr, "B")
    inv = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 60, ay + 1900)
    span = _const(m, 1.0 / max(float(far_cm - near_cm), 1.0), ax - 100, ay + 2040)
    _lib.connect_material_expressions(fr, "", inv, "A")
    _lib.connect_material_expressions(span, "", inv, "B")
    nearm = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, ax + 220, ay + 1900)
    _lib.connect_material_expressions(inv, "", nearm, "")
    cap = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 380, ay + 1900)
    capk = _const(m, 0.6, ax + 220, ay + 2040)
    _lib.connect_material_expressions(nearm, "", cap, "A")
    _lib.connect_material_expressions(capk, "", cap, "B")
    blend = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, ax + 560, ay + 700)
    _lib.connect_material_expressions(coarse, "", blend, "A")
    _lib.connect_material_expressions(fine, "", blend, "B")
    _lib.connect_material_expressions(cap, "", blend, "Alpha")
    nrm = _lib.create_material_expression(m, unreal.MaterialExpressionNormalize, ax + 720, ay + 700)
    _lib.connect_material_expressions(blend, "", nrm, "")
    return nrm


def _waterline_wetness(m, mask_z, top_cm, power, ax, ay):
    """0..1 wet-sheen mask: 1 at the waterline (world Z 0), fading to 0 by top_cm up
    (Power concentrates it low). Keyed off WORLD Z (not local) so the band stays
    pinned to the real waterline as the hull bobs/rolls on the Gerstner surface
    (buoyancy drives the actor's world Z)."""
    top = _const(m, float(top_cm), ax, ay + 160)
    sub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, ax + 140, ay)
    _lib.connect_material_expressions(top, "", sub, "A")
    _lib.connect_material_expressions(mask_z, "", sub, "B")
    inv = _const(m, 1.0 / float(top_cm), ax, ay + 240)
    mul = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 280, ay)
    _lib.connect_material_expressions(sub, "", mul, "A")
    _lib.connect_material_expressions(inv, "", mul, "B")
    sat = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, ax + 420, ay)
    _lib.connect_material_expressions(mul, "", sat, "")
    powr = _lib.create_material_expression(m, unreal.MaterialExpressionPower, ax + 560, ay)
    pexp = _const(m, float(power), ax + 420, ay + 160)
    _lib.connect_material_expressions(sat, "", powr, "Base")
    _lib.connect_material_expressions(pexp, "", powr, "Exp")
    return powr


def _detail_rao(m, world_size_cm, ax, ay, tex="T_ShipDetail_RAO"):
    """Triplanar packed detail: returns (rough, ao, dirt) channel masks."""
    rao = _triplanar(m, _detail_tex(tex), world_size_cm,
                     unreal.MaterialSamplerType.SAMPLERTYPE_MASKS, ax, ay)

    def chan(r, g, b, yy):
        mk = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, ax + 460, yy)
        mk.set_editor_property("r", r)
        mk.set_editor_property("g", g)
        mk.set_editor_property("b", b)
        mk.set_editor_property("a", False)
        _lib.connect_material_expressions(rao, "", mk, "")
        return mk

    return chan(True, False, False, ay), chan(False, True, False, ay + 60), chan(False, False, True, ay + 120)


def _detail_rao_blend(m, world_size_cm, macro, ax, ay):
    """ANTI-TILING (P2-5): sample TWO baked RAO variations and macro-blend each channel —
    the rust/weathering pattern then differs per region instead of recurring every tile.
    Returns the blended (rough, ao, dirt)."""
    r1, a1, d1 = _detail_rao(m, world_size_cm, ax, ay, "T_ShipDetail_RAO")
    r2, a2, d2 = _detail_rao(m, world_size_cm, ax, ay + 1700, "T_ShipDetail_RAO2")

    def mix(c1, c2, yy):
        n = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, ax + 640, yy)
        _lib.connect_material_expressions(c1, "", n, "A")
        _lib.connect_material_expressions(c2, "", n, "B")
        _lib.connect_material_expressions(macro, "", n, "Alpha")
        return n

    return mix(r1, r2, ay), mix(a1, a2, ay + 60), mix(d1, d2, ay + 120)


def _macro_modulate(m, value, macro, lo, hi, ax, ay):
    """value * lerp(lo, hi, macro) — cheap per-region modulation (e.g. rust amount) so a
    single tiling detail stops reading as a uniform repeat (P2-5, the non-hull parts)."""
    amt = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, ax, ay)
    klo = _const(m, lo, ax - 150, ay + 140)
    khi = _const(m, hi, ax - 150, ay + 200)
    _lib.connect_material_expressions(klo, "", amt, "A")
    _lib.connect_material_expressions(khi, "", amt, "B")
    _lib.connect_material_expressions(macro, "", amt, "Alpha")
    out = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 160, ay)
    _lib.connect_material_expressions(value, "", out, "A")
    _lib.connect_material_expressions(amt, "", out, "B")
    return out


def _grime_gradient(m, mask_z, dirt, span_cm, lo, hi, ax, ay):
    """dirt * lerp(lo, hi, saturate(worldZ / span_cm)): concentrate grime toward the waterline.
    `lo` applies near z0 (dirtiest, salt/rust weeping up from the boot-topping), `hi` by span_cm
    up (cleaner topsides). World-Z keyed so the gradient tracks the bobbing hull — the triplanar-
    safe stand-in for vertical weather streaks (naval-AD: 'dirtier from the waterline up')."""
    inv = _const(m, 1.0 / float(span_cm), ax - 160, ay + 140)
    frac0 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax, ay)
    _lib.connect_material_expressions(mask_z, "", frac0, "A")
    _lib.connect_material_expressions(inv, "", frac0, "B")
    frac = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, ax + 140, ay)
    _lib.connect_material_expressions(frac0, "", frac, "")
    gmul = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, ax + 280, ay)
    klo = _const(m, lo, ax + 140, ay + 140)
    khi = _const(m, hi, ax + 140, ay + 220)
    _lib.connect_material_expressions(klo, "", gmul, "A")
    _lib.connect_material_expressions(khi, "", gmul, "B")
    _lib.connect_material_expressions(frac, "", gmul, "Alpha")
    out = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 440, ay)
    _lib.connect_material_expressions(dirt, "", out, "A")
    _lib.connect_material_expressions(gmul, "", out, "B")
    return out


def _cavity_ao(m, base, floor, ax, ay):
    """base * lerp(floor, 1, VertexColor): tempered baked cavity AO. The frigate's LOD0 carries a
    baked AO vertex color (`frigate.cavity_ao`); only the deepest geometry recesses darken (to
    `floor`x), open faces stay 1x — the value-break that stops the hull reading as flat 'putty'
    (naval-AD). Meshes WITHOUT the AO vertex color read white (1) => unaffected (missile/rocket)."""
    vc = _lib.create_material_expression(m, unreal.MaterialExpressionVertexColor, ax - 420, ay + 220)
    # CONTRAST the baked AO so only the DEEP junctions stay dark and the broad face gradient lifts
    # to white — pinches the AO into tight recess LINES instead of a blobby quilt (naval-AD 5: the
    # cuts=1 bake read as soft dark dots). saturate((VC - 0.25) * 1.7): maps [0.25,0.84] -> [0,1].
    sub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, ax - 300, ay + 220)
    clip = _const(m, 0.25, ax - 420, ay + 320)
    _lib.connect_material_expressions(vc, "", sub, "A")
    _lib.connect_material_expressions(clip, "", sub, "B")
    scaled = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax - 200, ay + 220)
    kk = _const(m, 1.7, ax - 300, ay + 320)
    _lib.connect_material_expressions(sub, "", scaled, "A")
    _lib.connect_material_expressions(kk, "", scaled, "B")
    vc_c = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, ax - 100, ay + 220)
    _lib.connect_material_expressions(scaled, "", vc_c, "")
    lo = _const3(m, floor, floor, floor, ax - 220, ay + 360)
    hi = _const3(m, 1.0, 1.0, 1.0, ax - 220, ay + 430)
    remap = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, ax - 60, ay + 260)
    _lib.connect_material_expressions(lo, "", remap, "A")
    _lib.connect_material_expressions(hi, "", remap, "B")
    _lib.connect_material_expressions(vc_c, "", remap, "Alpha")
    out = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 120, ay)
    _lib.connect_material_expressions(base, "", out, "A")
    _lib.connect_material_expressions(remap, "", out, "B")
    return out


def _weathered_basecolor(m, macro, ao, dirt, ax, ay):
    """macro paint x AO (dark seams) x rust/dirt tint -> MP_BASE_COLOR input."""
    ao_term = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, ax, ay)
    ao_lo = _const(m, 0.62, ax - 160, ay + 140)  # P3-7g: deeper seam/recess darkening in albedo (naval-AD 4:
    #                                               break the "putty monolith" — plate seams read as panel-line VALUE)
    ao_hi = _const(m, 1.0, ax - 160, ay + 200)
    _lib.connect_material_expressions(ao_lo, "", ao_term, "A")
    _lib.connect_material_expressions(ao_hi, "", ao_term, "B")
    _lib.connect_material_expressions(ao, "", ao_term, "Alpha")

    rust = _const3(m, 0.32, 0.20, 0.14, ax - 160, ay + 300)
    white = _const3(m, 1.0, 1.0, 1.0, ax - 160, ay + 360)
    dirt06 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax - 160, ay + 440)
    d6 = _const(m, 0.6, ax - 320, ay + 480)
    _lib.connect_material_expressions(dirt, "", dirt06, "A")
    _lib.connect_material_expressions(d6, "", dirt06, "B")
    dirt_tint = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, ax, ay + 320)
    _lib.connect_material_expressions(white, "", dirt_tint, "A")
    _lib.connect_material_expressions(rust, "", dirt_tint, "B")
    _lib.connect_material_expressions(dirt06, "", dirt_tint, "Alpha")

    c1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 200, ay)
    _lib.connect_material_expressions(macro, "", c1, "A")
    _lib.connect_material_expressions(ao_term, "", c1, "B")
    c2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 340, ay + 120)
    _lib.connect_material_expressions(c1, "", c2, "A")
    _lib.connect_material_expressions(dirt_tint, "", c2, "B")
    return c2


def _roughness_from_detail(m, base_rough, rough_detail, ax, ay):
    """Blend a painted-metal base roughness toward the weathered detail value."""
    base = _const(m, base_rough, ax, ay)
    out = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, ax + 160, ay)
    blend = _const(m, 0.8, ax, ay + 140)
    _lib.connect_material_expressions(base, "", out, "A")
    _lib.connect_material_expressions(rough_detail, "", out, "B")
    _lib.connect_material_expressions(blend, "", out, "Alpha")
    return out


def _hull_decal_mask(m, world, mask_z):
    """World-projected hull-markings paint mask (S2 human-scale decal). The frigate sits at
    STAGE_ORIGIN (300000,300000) facing +Y, ~150 m along Y. Map world-Y -> U (bow +Y at U~0,
    stern toward U~1) and world-Z -> V (top->bottom), sample T_HullMarkings (CLAMP addressing =
    one appearance, black border outside [0,1]). Returns the R-channel mask 0..1: 1 where the
    pennant number / draft ladder / hatch stencil paints. Window values tuned to the hull."""
    yy = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -1200, 1100)
    yy.set_editor_property("r", False)
    yy.set_editor_property("g", True)
    yy.set_editor_property("b", False)
    yy.set_editor_property("a", False)
    _lib.connect_material_expressions(world, "", yy, "")
    # ASPECT-MATCHED window: texture is 1024x256 (4:1), so the world window must be 4:1 (Yspan =
    # 4 x Zspan) or the marks stretch. 20 m (Y) x 5 m (Z) forward-hull patch -> the "81" reads as a
    # ~4.4 m x 2.3 m number, draft ladder at the stem, hatches aft, all undistorted on the topside.
    usub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -1040, 1100)
    _lib.connect_material_expressions(_const(m, 303000.0, -1200, 1240), "", usub, "A")  # window Y 301000..303000 (forward hull)
    _lib.connect_material_expressions(yy, "", usub, "B")
    u = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -880, 1100)
    _lib.connect_material_expressions(usub, "", u, "A")
    _lib.connect_material_expressions(_const(m, 1.0 / 2000.0, -1040, 1240), "", u, "B")  # 20 m span
    vsub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -1040, 1320)
    _lib.connect_material_expressions(_const(m, 540.0, -1200, 1460), "", vsub, "A")
    _lib.connect_material_expressions(mask_z, "", vsub, "B")
    v = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -880, 1320)
    _lib.connect_material_expressions(vsub, "", v, "A")
    _lib.connect_material_expressions(_const(m, 1.0 / 500.0, -1040, 1460), "", v, "B")  # 5 m span (Z 40..540)
    uv = _lib.create_material_expression(m, unreal.MaterialExpressionAppendVector, -720, 1200)
    _lib.connect_material_expressions(u, "", uv, "A")
    _lib.connect_material_expressions(v, "", uv, "B")
    ts = _lib.create_material_expression(m, unreal.MaterialExpressionTextureSample, -560, 1200)
    ts.set_editor_property("texture", _detail_tex("T_HullMarkings"))
    ts.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
    _lib.connect_material_expressions(uv, "", ts, "UVs")
    markr = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -380, 1200)
    markr.set_editor_property("r", True)
    markr.set_editor_property("g", False)
    markr.set_editor_property("b", False)
    markr.set_editor_property("a", False)
    _lib.connect_material_expressions(ts, "", markr, "")
    return markr


def make_naval_hull():
    """Haze gray topsides, dark antifouling below the waterline (world Z 0),
    with a black boot-topping band — the classic warship paint scheme, driven
    purely by world height so it works on any hull the generators emit."""
    m = _new_material("M_NavalHull")
    haze = _const3(m, 0.085, 0.097, 0.107, -900, -200)  # P3-7.3: hull MID tier pushed darker for a wider value gap vs the 0.19 superstructure (naval-AD "one flat value")
    antifoul = _const3(m, 0.110, 0.030, 0.024, -900, 0)  # dark-red anti-fouling below the waterline
    boot = _const3(m, 0.018, 0.020, 0.023, -900, 200)    # near-black boot-topping stripe

    world = _lib.create_material_expression(m, unreal.MaterialExpressionWorldPosition, -1200, 400)
    mask_z = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -1050, 400)
    mask_z.set_editor_property("r", False)
    mask_z.set_editor_property("g", False)
    mask_z.set_editor_property("b", True)
    _lib.connect_material_expressions(world, "", mask_z, "")

    # below = saturate((25 - z) / 30): 1 under the boot band, 0 above it.
    offset = _const(m, 25.0, -1050, 560)
    sub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -900, 480)
    _lib.connect_material_expressions(offset, "", sub, "A")
    _lib.connect_material_expressions(mask_z, "", sub, "B")
    scale = _const(m, 1.0 / 30.0, -900, 640)
    mul = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -750, 520)
    _lib.connect_material_expressions(sub, "", mul, "A")
    _lib.connect_material_expressions(scale, "", mul, "B")
    below = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -620, 520)
    _lib.connect_material_expressions(mul, "", below, "")

    # boot band = saturate((140 - z) / 25) - below: a ~1.3 m black boot-topping stripe at the
    # waterline (naval-AD: "the single most recognizable 'real warship' cue, and it's absent").
    offset2 = _const(m, 175.0, -1050, 760)  # P3-7.3: taller (~1.5 m) boot-topping stripe so the waterline reads
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

    # Triplanar PBR detail over the world-Z paint scheme: panel/plate/weld normal,
    # weathering roughness, gentle seam AO + sparse rust. 6.5 m tile = ~1.6 m plates
    # (architectural plating, not corrugation).
    # ANTI-TILING (P2-5): blend TWO baked detail variations by a large-scale static macro
    # mask so the hull's plate/weld/rust pattern differs per region instead of recurring
    # every ~6.5 m triplanar tile (the biggest "CG repeat" tell up close).
    macro = _macro_variation(m, -1750, -700)
    # P3-2: tighter triplanar tile (650→420 cm) ~1.5x the texel density / plating frequency so
    # the panels read as plating up close instead of mip-blur (critics: "low-texel, flat"). The
    # P2-5 macro/2-variation blend keeps the tighter tile from re-introducing a visible repeat.
    rough_d, ao_d, dirt_d = _detail_rao_blend(m, 320.0, macro, 700, -240)  # P3-7.S1b: match the NORMAL tile (320) so AO seams and plate-line seams register at the SAME frequency (naval-AD: 420 vs 320 mismatch)
    # Strengthen the regional weathering: real warships rust/stain UNEVENLY (streaks below
    # fittings, salt-faded patches) — push the rust hard by region so the hull looks
    # lived-in, not a uniform clean coat. This both de-tiles and adds the realism the
    # uniform tiling lacked (the visible payoff of the macro mask).
    dirt_d = _macro_modulate(m, dirt_d, macro, 0.25, 1.85, 1360, -440)
    # P3-7d: concentrate the grime toward the waterline (1.4x at z0 -> 0.8x up the topsides) so the
    # hull reads lived-in (salt/rust weeping up from the boot-topping), not a uniform clean coat.
    dirt_d = _grime_gradient(m, mask_z, dirt_d, 550.0, 1.95, 0.75, 1360, -560)  # P3-7f: push the near-waterline rust (naval-AD 3: grime was a no-op)
    base = _weathered_basecolor(m, lerp2, ao_d, dirt_d, 1600, -240)
    rough = _roughness_from_detail(m, 0.55, rough_d, 1600, 240)

    # Waterline wetness: a wet-sheen band pinned to the real waterline (world Z 0,
    # so it tracks the hull as buoyancy bobs/rolls it). Where wet, the paint goes
    # DARKER (water-soaked) and far GLOSSIER (low roughness) — the single biggest
    # "real ship on real water" tell, and it reads hardest at the low/grazing camera.
    # P3-7/Phase-7 (naval-AD re-judge): the raking key made the hard flat waterline CUT obvious —
    # no wet band read. Widen the soaked zone (260->400 cm up the hull) and soften the falloff
    # (power 1.7->1.2) so a clearly darker, glossier wet band climbs the topside; the raked light
    # is exactly what sells it now (biggest "real ship on real water" cue).
    # P3-7.3 (naval-AD re-judge: wet band STILL not reading at 400/1.2 — it overlapped the dark
    # boot/antifoul and only touched a thin gray strip): climb it well up the visible gray hull
    # (400->650 cm) and darken the soak harder (0.60->0.50) so a substantial darker/glossier wet
    # lower-hull reads under the raked key — the biggest remaining "real ship on water" cue.
    wet = _waterline_wetness(m, mask_z, 650.0, 1.1, 2150, 560)
    dry_tint = _const3(m, 1.0, 1.0, 1.0, 2150, 980)
    wet_tint = _const3(m, 0.50, 0.52, 0.56, 2150, 1040)  # wet paint darkens ~0.5x (deeper soak)
    tint = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, 2320, 940)
    _lib.connect_material_expressions(dry_tint, "", tint, "A")
    _lib.connect_material_expressions(wet_tint, "", tint, "B")
    _lib.connect_material_expressions(wet, "", tint, "Alpha")
    base_wet = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, 2480, 0)
    _lib.connect_material_expressions(base, "", base_wet, "A")
    _lib.connect_material_expressions(tint, "", base_wet, "B")
    base_final = _cavity_ao(m, base_wet, 0.68, 2640, 0)  # P3-A: baked cavity AO breaks the monolith
    # S2 human-scale decal: paint the hull markings (number/draft/hatches) white where the
    # world-projected stencil hits — the scale anchor naval-AD flagged as the #1 lever.
    decal = _hull_decal_mask(m, world, mask_z)
    white_paint = _const3(m, 0.86, 0.87, 0.90, 2640, 700)
    base_marked = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, 2820, 0)
    _lib.connect_material_expressions(base_final, "", base_marked, "A")
    _lib.connect_material_expressions(white_paint, "", base_marked, "B")
    _lib.connect_material_expressions(decal, "", base_marked, "Alpha")
    _lib.connect_material_property(base_marked, "", unreal.MaterialProperty.MP_BASE_COLOR)

    wet_amt = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, 2320, 300)
    wet_k = _const(m, 0.85, 2150, 360)  # how far toward fully-wet the roughness drops
    _lib.connect_material_expressions(wet, "", wet_amt, "A")
    _lib.connect_material_expressions(wet_k, "", wet_amt, "B")
    rough_wet = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, 2480, 240)
    wet_rough = _const(m, 0.09, 2320, 460)  # wet metal is near-mirror
    _lib.connect_material_expressions(rough, "", rough_wet, "A")
    _lib.connect_material_expressions(wet_rough, "", rough_wet, "B")
    _lib.connect_material_expressions(wet_amt, "", rough_wet, "Alpha")
    # painted markings read a touch matte vs the wet hull
    rough_marked = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, 2820, 240)
    _lib.connect_material_expressions(rough_wet, "", rough_marked, "A")
    _lib.connect_material_expressions(_const(m, 0.42, 2640, 360), "", rough_marked, "B")
    _lib.connect_material_expressions(decal, "", rough_marked, "Alpha")
    _lib.connect_material_property(rough_marked, "", unreal.MaterialProperty.MP_ROUGHNESS)

    # P3-7: painted hull is dielectric (0), but WORN/rusted areas (dirt mask) read as bare
    # metal showing through the coating — the painted-vs-metal split the critics wanted.
    metal = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, 1600, 420)
    metal_k = _const(m, 0.35, 1600, 520)
    _lib.connect_material_expressions(dirt_d, "", metal, "A")
    _lib.connect_material_expressions(metal_k, "", metal, "B")
    _lib.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    # Coarse plate relief everywhere + a finer sub-panel tile that fades in within
    # ~140 m (PixelDepth) for crisp close-up detail without far-field shimmer. The COARSE
    # band is macro-blended between the two baked variations (P2-5) so the plate layout
    # de-tiles; the fine band stays single-variation (keeps the cost down).
    nrm = _detail_normal_db(m, 320.0, 120.0, 2000.0, 22000.0, 700, 560,  # P3-7.S1b: hold the FINE sub-panel band out to ~220m (was 140m) so plate/weld relief still reads at the ~110m hero distance (naval-AD: real fix is longer-holding fine band/mip-bias, not more tile tightening); cap (0.6) keeps far-field shimmer down
                            tex2="T_ShipDetail_N2", macro=macro)
    _lib.connect_material_property(nrm, "", unreal.MaterialProperty.MP_NORMAL)

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def _deck_decal_mask(m):
    """Top-down world-projected deck-markings mask (S2 polish — naval-AD gap #3: the main deck
    reads as an empty plane from above). Frigate at STAGE_ORIGIN (300000,300000); deck ~110 m (Y)
    x ~14 m (X). world-Y -> U (bow +Y at U~0, stern at U~1), world-X -> V (beam). CLAMP -> one
    appearance. Returns the R-channel paint mask 0..1 (helo circle/non-skid/hatches)."""
    world = _lib.create_material_expression(m, unreal.MaterialExpressionWorldPosition, -1200, 1700)
    yy = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -1040, 1700)
    yy.set_editor_property("r", False)
    yy.set_editor_property("g", True)
    yy.set_editor_property("b", False)
    yy.set_editor_property("a", False)
    _lib.connect_material_expressions(world, "", yy, "")
    usub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -880, 1700)
    _lib.connect_material_expressions(_const(m, 305000.0, -1040, 1840), "", usub, "A")
    _lib.connect_material_expressions(yy, "", usub, "B")
    u = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -720, 1700)
    _lib.connect_material_expressions(usub, "", u, "A")
    _lib.connect_material_expressions(_const(m, 1.0 / 11000.0, -880, 1840), "", u, "B")
    xx = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -1040, 1920)
    xx.set_editor_property("r", True)
    xx.set_editor_property("g", False)
    xx.set_editor_property("b", False)
    xx.set_editor_property("a", False)
    _lib.connect_material_expressions(world, "", xx, "")
    vsub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -880, 1920)
    _lib.connect_material_expressions(xx, "", vsub, "A")
    _lib.connect_material_expressions(_const(m, 299300.0, -1040, 2060), "", vsub, "B")
    v = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -720, 1920)
    _lib.connect_material_expressions(vsub, "", v, "A")
    _lib.connect_material_expressions(_const(m, 1.0 / 1400.0, -880, 2060), "", v, "B")
    uv = _lib.create_material_expression(m, unreal.MaterialExpressionAppendVector, -560, 1800)
    _lib.connect_material_expressions(u, "", uv, "A")
    _lib.connect_material_expressions(v, "", uv, "B")
    ts = _lib.create_material_expression(m, unreal.MaterialExpressionTextureSample, -400, 1800)
    ts.set_editor_property("texture", _detail_tex("T_DeckMarkings"))
    ts.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_MASKS)
    _lib.connect_material_expressions(uv, "", ts, "UVs")
    markr = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -240, 1800)
    markr.set_editor_property("r", True)
    markr.set_editor_property("g", False)
    markr.set_editor_property("b", False)
    markr.set_editor_property("a", False)
    _lib.connect_material_expressions(ts, "", markr, "")
    return markr


def make_detailed(name, rgb, base_rough, metallic, tile_cm=300.0, deck_decal=False):
    """Painted/metal hard-surface material: a constant macro colour with triplanar
    PBR detail (panel normal + weathering roughness + seam AO + rust). Turns a
    flat-shaded primitive into believable painted metal — no UVs (world triplanar).
    deck_decal=True overlays the top-down deck-markings stencil (M_NavalGray = deck)."""
    m = _new_material(name)
    macro = _const3(m, *rgb, -1100, -240)
    rough_d, ao_d, dirt_d = _detail_rao(m, tile_cm, 200, -240)
    # ANTI-TILING (P2-5, cheap path): modulate the rust/dirt per region by a static macro
    # noise so the single tiling detail stops reading as a uniform repeat. (The hull uses
    # the fuller 2-variation blend; these smaller parts get the cheap 1-sample macro.)
    macrovar = _macro_variation(m, -1100, 760)
    dirt_d = _macro_modulate(m, dirt_d, macrovar, 0.25, 1.8, -680, 760)
    base = _weathered_basecolor(m, macro, ao_d, dirt_d, 1100, -240)
    base = _cavity_ao(m, base, 0.68, 1320, -240)  # P3-A: baked cavity AO (superstructure/deck monolith break)
    rough = _roughness_from_detail(m, base_rough, rough_d, 1100, 240)
    if deck_decal:
        decal = _deck_decal_mask(m)
        white = _const3(m, 0.78, 0.79, 0.82, 1320, 120)  # painted deck markings (white non-skid/helo)
        bmix = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, 1500, -200)
        _lib.connect_material_expressions(base, "", bmix, "A")
        _lib.connect_material_expressions(white, "", bmix, "B")
        _lib.connect_material_expressions(decal, "", bmix, "Alpha")
        base = bmix
        rmix = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, 1500, 240)
        _lib.connect_material_expressions(rough, "", rmix, "A")
        _lib.connect_material_expressions(_const(m, 0.5, 1320, 360), "", rmix, "B")
        _lib.connect_material_expressions(decal, "", rmix, "Alpha")
        rough = rmix
    _lib.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    _lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    metal = _const(m, metallic, 1100, 420)
    _lib.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    nrm = _detail_normal(m, tile_cm, 200, 560)
    _lib.connect_material_property(nrm, "", unreal.MaterialProperty.MP_NORMAL)
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
    """Lit volumetric rocket-exhaust smoke (replaces the old flat unlit ribbon
    that read as a toy paper streamer). Four things sell it as real exhaust:
      - LIT translucency (volumetric non-directional) so the column takes sun/sky
        light and gains 3D form instead of a flat white sheet;
      - a DENSITY/AGE color gradient (dark hot core -> pale dissipated smoke),
        driven by the vertex-color alpha SeaWorldManager writes per trail point;
      - NOISE EROSION (world-space, time-panned) that breaks the uniform sheet
        into wispy turbulent edges;
      - EDGE SOFTNESS across the ribbon width so the long sides fade instead of
        cutting a hard line.
    All code-authored (MaterialEditingLibrary), no texture asset."""
    m = _new_material("M_RocketTrail")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    m.set_editor_property(
        "translucency_lighting_mode",
        unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_NON_DIRECTIONAL,
    )
    m.set_editor_property("two_sided", True)

    vcolor = _lib.create_material_expression(m, unreal.MaterialExpressionVertexColor, -1000, -200)

    # Base colour: dense/young = dark smoke, thin/old = pale smoke (lerp by the
    # per-point density in vertex alpha).
    light = _const3(m, 0.66, 0.66, 0.70, -1000, -360)
    dark = _const3(m, 0.20, 0.21, 0.24, -1000, -520)
    basecolor = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -760, -400)
    _lib.connect_material_expressions(light, "", basecolor, "A")
    _lib.connect_material_expressions(dark, "", basecolor, "B")
    _lib.connect_material_expressions(vcolor, "A", basecolor, "Alpha")
    _lib.connect_material_property(basecolor, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # Edge softness across the ribbon width: 1 - (|U-0.5|*2)^1.5  (0 at the long
    # edges, 1 down the centre line).
    uv = _lib.create_material_expression(m, unreal.MaterialExpressionTextureCoordinate, -1000, 120)
    umask = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -850, 120)
    umask.set_editor_property("r", True)
    umask.set_editor_property("g", False)
    umask.set_editor_property("b", False)
    umask.set_editor_property("a", False)
    _lib.connect_material_expressions(uv, "", umask, "")
    uhalf = _const(m, 0.5, -850, 250)
    usub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -700, 140)
    _lib.connect_material_expressions(umask, "", usub, "A")
    _lib.connect_material_expressions(uhalf, "", usub, "B")
    uabs = _lib.create_material_expression(m, unreal.MaterialExpressionAbs, -560, 140)
    _lib.connect_material_expressions(usub, "", uabs, "")
    utwo = _const(m, 2.0, -560, 250)
    umul = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -430, 150)
    _lib.connect_material_expressions(uabs, "", umul, "A")
    _lib.connect_material_expressions(utwo, "", umul, "B")
    upow = _lib.create_material_expression(m, unreal.MaterialExpressionPower, -300, 150)
    upow.set_editor_property("const_exponent", 1.5)
    _lib.connect_material_expressions(umul, "", upow, "Base")
    edge = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -170, 150)
    _lib.connect_material_expressions(upow, "", edge, "")

    # World-space, time-panned noise -> erode opacity into wisps.
    worldpos = _lib.create_material_expression(m, unreal.MaterialExpressionWorldPosition, -1000, 460)
    timev = _lib.create_material_expression(m, unreal.MaterialExpressionTime, -1000, 600)
    panvec = _const3(m, 9.0, 4.0, 16.0, -1000, 700)
    pan = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -850, 620)
    _lib.connect_material_expressions(timev, "", pan, "A")
    _lib.connect_material_expressions(panvec, "", pan, "B")
    npos = _lib.create_material_expression(m, unreal.MaterialExpressionAdd, -700, 500)
    _lib.connect_material_expressions(worldpos, "", npos, "A")
    _lib.connect_material_expressions(pan, "", npos, "B")
    noise = _lib.create_material_expression(m, unreal.MaterialExpressionNoise, -540, 500)
    noise.set_editor_property("scale", 0.0025)  # ~4 m feature size (default Simplex-tex noise)
    noise.set_editor_property("levels", 2)
    noise.set_editor_property("output_min", 0.0)
    noise.set_editor_property("output_max", 1.0)
    noise.set_editor_property("turbulence", True)
    _lib.connect_material_expressions(npos, "", noise, "Position")
    # contrast the noise so it bites: saturate((n - 0.30) * 2.2)
    nsub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -380, 500)
    ncut = _const(m, 0.30, -540, 660)
    _lib.connect_material_expressions(noise, "", nsub, "A")
    _lib.connect_material_expressions(ncut, "", nsub, "B")
    nmul = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -250, 500)
    ngain = _const(m, 2.2, -380, 660)
    _lib.connect_material_expressions(nsub, "", nmul, "A")
    _lib.connect_material_expressions(ngain, "", nmul, "B")
    nero = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -120, 500)
    _lib.connect_material_expressions(nmul, "", nero, "")

    # opacity = baseOpacity * density(vcolor.A) * edge * erosion
    base_op = _const(m, 1.4, -700, 320)
    o1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -560, 300)
    _lib.connect_material_expressions(base_op, "", o1, "A")
    _lib.connect_material_expressions(vcolor, "A", o1, "B")
    o2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -120, 300)
    _lib.connect_material_expressions(o1, "", o2, "A")
    _lib.connect_material_expressions(edge, "", o2, "B")
    o3 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, 40, 360)
    _lib.connect_material_expressions(o2, "", o3, "A")
    _lib.connect_material_expressions(nero, "", o3, "B")
    osat = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, 180, 360)
    _lib.connect_material_expressions(o3, "", osat, "")
    _lib.connect_material_property(osat, "", unreal.MaterialProperty.MP_OPACITY)

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def make_splash():
    """Aerated water column thrown up where a shell/rocket strikes the sea — a
    soft, noise-eroded spray, not a solid cylinder. A vertical gradient (dense
    aerated foot -> thin white spray crown), turbulence breaking the column into
    droplets, and a Fresnel silhouette feather so the sides melt into spray."""
    m = _new_material("M_Splash")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided", True)

    # V runs up the column (cylinder side UV): 0 = foot, 1 = crown.
    uv = _lib.create_material_expression(m, unreal.MaterialExpressionTextureCoordinate, -1000, 120)
    vmask = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -850, 120)
    vmask.set_editor_property("r", False)
    vmask.set_editor_property("g", True)
    vmask.set_editor_property("b", False)
    vmask.set_editor_property("a", False)
    _lib.connect_material_expressions(uv, "", vmask, "")

    foot = _const3(m, 0.80, 0.90, 0.96, -700, -260)   # aerated blue-white
    crown = _const3(m, 0.95, 0.97, 1.00, -700, -120)  # bright white spray
    color = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -480, -200)
    _lib.connect_material_expressions(foot, "", color, "A")
    _lib.connect_material_expressions(crown, "", color, "B")
    _lib.connect_material_expressions(vmask, "", color, "Alpha")
    _lib.connect_material_property(color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # Top thinning: (1 - V)^0.7 -> dense foot, wispy crown.
    vfade_inv = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -700, 140)
    _lib.connect_material_expressions(vmask, "", vfade_inv, "")
    vfade = _lib.create_material_expression(m, unreal.MaterialExpressionPower, -560, 140)
    vfade.set_editor_property("const_exponent", 0.7)
    _lib.connect_material_expressions(vfade_inv, "", vfade, "Base")

    noise = _panned_noise(m, 0.006, (2.0, 2.0, 9.0), 0.28, 2.0, ax=-560, ay=600)

    # Fresnel silhouette feather: keep facing, fade grazing edges (1 - 0.7*F).
    fres = _fresnel(m, 2.5, -700, 360)
    fres_s = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -560, 360)
    fres_k = _const(m, 0.7, -700, 480)
    _lib.connect_material_expressions(fres, "", fres_s, "A")
    _lib.connect_material_expressions(fres_k, "", fres_s, "B")
    feather = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -420, 360)
    _lib.connect_material_expressions(fres_s, "", feather, "")

    base_op = _const(m, 1.1, -700, 240)
    o1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -300, 200)
    _lib.connect_material_expressions(base_op, "", o1, "A")
    _lib.connect_material_expressions(vfade, "", o1, "B")
    o2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -150, 260)
    _lib.connect_material_expressions(o1, "", o2, "A")
    _lib.connect_material_expressions(noise, "", o2, "B")
    o3 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, 10, 320)
    _lib.connect_material_expressions(o2, "", o3, "A")
    _lib.connect_material_expressions(feather, "", o3, "B")
    osat = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, 160, 320)
    _lib.connect_material_expressions(o3, "", osat, "")
    _lib.connect_material_property(osat, "", unreal.MaterialProperty.MP_OPACITY)

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
    # The water zone assigns this as its far-distance mesh material, which REQUIRES
    # the "Used with Water" usage flag — without it UE silently falls back to the
    # default material in-game (the far ring renders wrong) and warns every launch.
    m.set_editor_property("used_with_water", True)
    # P3-1: the near-black distant sheet (0.012,0.032,0.045) was what the low-camera wave
    # crests REFLECTED → the "black oily ribbon" smears the critics flagged, AND a hard dark
    # horizon band. A muted sky-lit distant-sea blue-grey fixes both (crests now reflect a
    # believable horizon, and the sea-sky seam softens toward aerial perspective).
    color = _const3(m, 0.125, 0.165, 0.205, -450, 0)  # P3-7.2: lift again — near crests reflect a LIT horizon, calming the grazing-mirror contrast (naval-AD #2)
    _lib.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = _const(m, 0.15, -450, 220)
    _lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    spec = _const(m, 0.25, -450, 340)
    _lib.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)
    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def _ocean_instance(name, parent_path, overrides, vec_overrides=None):
    """A Material Instance of a Water-plugin ocean material with scalar (+ optional
    vector) overrides — inherits the plugin's waves/depth-color/foam/Gerstner wiring
    unchanged and only retunes the named parameters."""
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    path = f"{MAT_DIR}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    parent = unreal.load_asset(parent_path)
    if parent is None:
        raise RuntimeError(f"missing water parent {parent_path}")
    mi = tools.create_asset(
        name, MAT_DIR, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew()
    )
    mi.set_editor_property("parent", parent)
    for key, value in overrides.items():
        _lib.set_material_instance_scalar_parameter_value(mi, key, value)
    for key, (r, g, b) in (vec_overrides or {}).items():
        _lib.set_material_instance_vector_parameter_value(mi, key, unreal.LinearColor(r, g, b, 1.0))
    unreal.EditorAssetLibrary.save_loaded_asset(mi)
    return mi


def make_sea_ocean():
    """Calmer distant ocean. The Water plugin's master material splits the wave
    normal into a NEAR and a DISTANT band; the stock distant band (35 m tile at
    strength 0.30/0.25) repeats into a visible pattern at the horizon, and the
    near-mirror roughness (0.02) aliases into shimmer at grazing angles. Flatten
    the distant normal, fade it harder by Fresnel toward the horizon, and raise
    roughness to a Lumen-safe floor for specular anti-aliasing. Authored as
    instances (main + LOD) so the surface keeps the plugin's waves and depth
    color — only the far-field look changes."""
    overrides = {
        # P2-6: the distant band was flattened so hard (0.12) that the MID-FIELD read as
        # glassy/plastic. Lift it a notch AND push the Fresnel power so the flatten is
        # confined to the TRUE horizon — mid-field regains wave relief, horizon stays
        # shimmer-free (the anti-patterning lever is the Fresnel falloff, not zero normal).
        "Default Distant Normal Strength": 0.16,   # 0.30 stock; 0.12 was too flat mid-field
        "Default Distant Normal StrengthB": 0.12,  # 0.25 stock
        "Far Normal Fresnel Power": 18.0,          # 14 — confine the flatten to the horizon
        # P3-6a — render-eng root-caused the black "oily ribbon" grazing reflections as a
        # near-MIRROR surface reflecting dark scene elements (NOT primarily SLW ray-miss).
        # Roughening the grazing reflection scatters those rays into a soft sheen instead of a
        # hard black streak — the single biggest cheap water win.
        "Water Roughness": 0.15,                   # P3-7.4: nudged up — calm the raked-light grazing shards
        "Water Fresnel Roughness": 0.38,           # P3-7.4: pushed hard (0.22->0.38) — the GRAZING-specific roughness blurs the black-white "cracked-glass" mirror shards into a soft glint field (naval-AD: last macro eyesore; SLW ceiling so calm not clear)
        # P2-6: the near band at 0.42 broke the foreground into noisy glints that the Lumen
        # reflection denoiser smeared into an 'oily' look. 0.34 keeps crisp near relief
        # without over-fracturing the close reflection (probed stock default 0.25).
        "Default Near Normal Strength": 0.60,      # P3-7g: render-eng 4 SLW go/no-go test — last cheap push at the
        #                                            grazing mirror before the SLW-escape decision (P3-7f: 0.46) — render-eng 3:
        #                                            the near field was 2 big smooth S-curves on glass; break
        #                                            it into many small glints (safe now at roughness 0.13)
        # Crest foam / whitecaps: bring the EXISTING Gerstner crests to life with
        # foam streaks WITHOUT raising wave amplitude (keeps the anti-shimmer + perf).
        # Param names from the Water_Material_Ocean probe.
        # P3-1: whitecaps READ now (critics: "zero foam, ship sits in water like a prop").
        # Stronger boost + much lower height threshold puts foam streaks on far more crests —
        # which also breaks up the dark crest reflections that read as smears.
        "Foam Opacity": 1.0,
        "Foam Boost": 5.0,                         # 3.2 — crests break into readable whitecaps
        "Height Bias": 0.06,                       # 0.12 — foam gate lower still (sea is near-glassy)
        # P3-7g — render-eng 3: the foam read as "thin bright specular streaks / chrome trim", not
        # whitewater. The plugin exposes Foam Roughness — the foam was glossy. Make it MATTE (0.9)
        # so crests read as opaque diffuse foam, and soften the rim so it's broad, not a 1-px line.
        "Foam Roughness": 0.90,                    # matte whitewater (was glossy -> specular glint)
        "FoamContrast": 0.24,                      # 0.34 — broader, softer foam edge
    }
    # P2-2: subtle sub-surface scatter tint (probed default white (1,1,1)) — a gentle
    # teal so the water reads with translucent DEPTH, not a flat sheet. Depth COLOR still
    # comes from the untouched Absorption (10,150,350); this only tints the scatter. Kept
    # subtle to avoid a murky/green regression — verified by capture, reverted if it reads off.
    vec_overrides = {
        # P3-4: the bright teal scatter read as "swimming-pool", not open ocean (critics).
        # Shift toward a muted deep blue-green (less green, dimmer) so the sea reads as deep
        # water with a believable depth gradient instead of a uniform chroma-heavy cyan.
        # P3-7f — render-eng 3: (0.12,0.24,0.32) over-corrected into "black glass" — the foreground
        # had no luminous scatter FLOOR, so un-reflected grazing pixels went black. Lift the scatter
        # to a luminous deep blue-teal (brighter than the petrol floor, still well below the old
        # pool-cyan 0.40,0.66,0.76) so the foreground sits on lit water and the ribbons read as
        # glints on a sea, not smears on a void. This is the highest-leverage cheap water fix.
        "Scattering": (0.18, 0.36, 0.48),
    }
    _ocean_instance("MI_SeaOcean", "/Water/Materials/WaterSurface/Water_Material_Ocean",
                    overrides, vec_overrides)
    _ocean_instance(
        "MI_SeaOceanLOD", "/Water/Materials/WaterSurface/LODs/Water_Material_Ocean_LOD",
        overrides, vec_overrides
    )


def make_burst():
    """Airburst — a flak puff, not a grey ball. A hot core flashes at detonation
    then decays to dark powder smoke (driven by the C++-set 'Age' 0..1 scalar);
    Fresnel blends the bright core out to a dark grazing edge and feathers the
    silhouette; turbulence erodes it into a ragged cloud that thins as it ages."""
    m = _new_material("M_Burst")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided", True)

    age = _lib.create_material_expression(m, unreal.MaterialExpressionScalarParameter, -1100, 700)
    age.set_editor_property("parameter_name", "Age")
    age.set_editor_property("default_value", 0.0)

    fres = _fresnel(m, 2.0, -1100, 300)
    flash = _const3(m, 2.8, 1.30, 0.45, -900, -300)   # ignition flash (blooms harder)
    smoke = _const3(m, 0.06, 0.06, 0.07, -900, -120)  # dark powder smoke

    # flashAmt = saturate(1 - Age*7): hot only in the first ~1/7 of life.
    fa_mul = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -900, 560)
    fa_k = _const(m, 7.0, -1050, 660)
    _lib.connect_material_expressions(age, "", fa_mul, "A")
    _lib.connect_material_expressions(fa_k, "", fa_mul, "B")
    fa_inv = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -760, 560)
    _lib.connect_material_expressions(fa_mul, "", fa_inv, "")
    flashamt = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -620, 560)
    _lib.connect_material_expressions(fa_inv, "", flashamt, "")

    # center = lerp(smoke, flash, flashAmt); emissive = lerp(center, smoke, Fresnel)
    center = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -700, -240)
    _lib.connect_material_expressions(smoke, "", center, "A")
    _lib.connect_material_expressions(flash, "", center, "B")
    _lib.connect_material_expressions(flashamt, "", center, "Alpha")
    emis = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -480, -200)
    _lib.connect_material_expressions(center, "", emis, "A")
    _lib.connect_material_expressions(smoke, "", emis, "B")
    _lib.connect_material_expressions(fres, "", emis, "Alpha")
    _lib.connect_material_property(emis, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    noise = _panned_noise(m, 0.0018, (5.0, 3.0, 5.0), 0.26, 2.2, ax=-560, ay=620)
    feather = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -300, 300)
    _lib.connect_material_expressions(fres, "", feather, "")
    agefade = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -300, 420)
    _lib.connect_material_expressions(age, "", agefade, "")  # thin out over life

    base_op = _const(m, 1.3, -700, 180)
    o1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -150, 200)
    _lib.connect_material_expressions(base_op, "", o1, "A")
    _lib.connect_material_expressions(noise, "", o1, "B")
    o2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, 10, 260)
    _lib.connect_material_expressions(o1, "", o2, "A")
    _lib.connect_material_expressions(feather, "", o2, "B")
    o3 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, 170, 320)
    _lib.connect_material_expressions(o2, "", o3, "A")
    _lib.connect_material_expressions(agefade, "", o3, "B")
    osat = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, 320, 320)
    _lib.connect_material_expressions(o3, "", osat, "")
    _lib.connect_material_property(osat, "", unreal.MaterialProperty.MP_OPACITY)

    # NOTE: a heat-haze refraction (per-pixel IOR wobble -> MP_REFRACTION) was tried here
    # for a hot-air shimmer around the fireball, but on this Metal/unlit-translucent path
    # it produced no confirmable distortion in close synthetic-burst captures (the hot
    # window is also too brief + intercepts too distant to ever read it). Removed rather
    # than ship an unverified effect — see perf-report §6.10 / task P2-3b for a future
    # validated retry (correct refraction_method + possibly a lit/translucent variant).

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def _instance_age(m, x, y):
    """Per-instance Age (0..1) from custom-data slot 0 — drives the particle's age
    fade now that puffs/debris/flashes draw through an InstancedStaticMeshComponent
    (SeaWorldManager writes SetCustomDataValue(i, 0, AgeFrac) per instance). Replaces
    the old per-actor 'Age' ScalarParameter; the downstream graph is unchanged."""
    node = _lib.create_material_expression(m, unreal.MaterialExpressionPerInstanceCustomData, x, y)
    node.set_editor_property("data_index", 0)  # custom-data slot 0 = Age (default 0 when unset)
    return node


def make_rocket_smoke():
    """Lit volumetric smoke PUFF for the billboard particle stream (replaces the
    flat 'toy' ribbon). The soft T_Smoke sprite gives real cloud shape; an 'Age'
    scalar (set per puff in C++) drives young-dark-sooty -> old-pale-dissipated
    colour, a brief hot-exhaust glow on the youngest puffs, and the opacity fade."""
    m = _new_material("M_RocketSmoke")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    m.set_editor_property("translucency_lighting_mode",
                          unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_NON_DIRECTIONAL)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("used_with_instanced_static_meshes", True)

    age = _instance_age(m, -1100, 500)
    tex = _lib.create_material_expression(m, unreal.MaterialExpressionTextureSample, -900, -220)
    tex.set_editor_property("texture", _detail_tex("T_Smoke"))
    tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)

    dark = _const3(m, 0.13, 0.13, 0.15, -700, -380)
    pale = _const3(m, 0.66, 0.66, 0.70, -700, -460)
    basecolor = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -480, -420)
    _lib.connect_material_expressions(dark, "", basecolor, "A")
    _lib.connect_material_expressions(pale, "", basecolor, "B")
    _lib.connect_material_expressions(age, "", basecolor, "Alpha")
    _lib.connect_material_property(basecolor, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # Hot exhaust glow ONLY at the nozzle: a brief, dim warm tip (saturate(1 - Age*14))
    # ~ the first 0.07 of life. Anything longer paints the whole launch column orange
    # so it reads as a string of embers instead of grey smoke.
    hot = _const3(m, 0.55, 0.24, 0.07, -700, 40)
    a5 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -760, 180)
    a5k = _const(m, 14.0, -900, 220)
    _lib.connect_material_expressions(age, "", a5, "A")
    _lib.connect_material_expressions(a5k, "", a5, "B")
    yinv = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -620, 180)
    _lib.connect_material_expressions(a5, "", yinv, "")
    young = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -500, 180)
    _lib.connect_material_expressions(yinv, "", young, "")
    glow = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -320, 80)
    _lib.connect_material_expressions(hot, "", glow, "A")
    _lib.connect_material_expressions(young, "", glow, "B")
    glow2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -140, 100)
    _lib.connect_material_expressions(glow, "", glow2, "A")
    _lib.connect_material_expressions(tex, "A", glow2, "B")
    _lib.connect_material_property(glow2, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # opacity = texAlpha * baseOp * (1-Age)^1.2
    afi = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -700, 400)
    _lib.connect_material_expressions(age, "", afi, "")
    afade = _lib.create_material_expression(m, unreal.MaterialExpressionPower, -560, 400)
    afade.set_editor_property("const_exponent", 1.2)
    _lib.connect_material_expressions(afi, "", afade, "Base")
    baseop = _const(m, 0.80, -700, 320)  # dense enough that neighbours MELD into one
                                         # column (too low and the cores show through
                                         # each other -> a granular 'popcorn' read)
    o1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -320, 340)
    _lib.connect_material_expressions(tex, "A", o1, "A")
    _lib.connect_material_expressions(baseop, "", o1, "B")
    o2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -160, 380)
    _lib.connect_material_expressions(o1, "", o2, "A")
    _lib.connect_material_expressions(afade, "", o2, "B")
    osat = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, 0, 380)
    _lib.connect_material_expressions(o2, "", osat, "")
    _lib.connect_material_property(osat, "", unreal.MaterialProperty.MP_OPACITY)

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def make_muzzle():
    """Launch flash — the T_Flash star sprite as a hot emissive billboard at the
    tube mouth. Over-1 emissive (blooms hard) faded over its brief life by the C++
    'Age' scalar; the sprite shape replaces the old plain bright ball."""
    m = _new_material("M_Muzzle")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("used_with_instanced_static_meshes", True)

    age = _instance_age(m, -1000, 300)
    ageinv = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -820, 300)
    _lib.connect_material_expressions(age, "", ageinv, "")

    tex = _lib.create_material_expression(m, unreal.MaterialExpressionTextureSample, -820, -160)
    tex.set_editor_property("texture", _detail_tex("T_Flash"))
    tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)

    bright = _const(m, 11.0, -620, -40)  # over-1 so the launch/detonation flash blooms hard
    e1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -460, -120)
    _lib.connect_material_expressions(tex, "", e1, "A")     # flash RGB
    _lib.connect_material_expressions(bright, "", e1, "B")
    e2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -300, -100)
    _lib.connect_material_expressions(e1, "", e2, "A")
    _lib.connect_material_expressions(ageinv, "", e2, "B")  # fade over life
    _lib.connect_material_property(e2, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    o1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -300, 200)
    _lib.connect_material_expressions(tex, "A", o1, "A")
    _lib.connect_material_expressions(ageinv, "", o1, "B")
    _lib.connect_material_property(o1, "", unreal.MaterialProperty.MP_OPACITY)

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def make_debris():
    """Glowing fragment spark flung from an intercept detonation — a hot ADDITIVE
    billboard (T_Flash star) that burns white-hot then cools to ember-orange and
    fades over its brief life (C++ 'Age' scalar). Additive so a radial spray of them
    reads as incandescent sparks punching through the fireball."""
    m = _new_material("M_Debris")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ADDITIVE)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("used_with_instanced_static_meshes", True)

    age = _instance_age(m, -1000, 300)
    ageinv = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -820, 300)
    _lib.connect_material_expressions(age, "", ageinv, "")

    tex = _lib.create_material_expression(m, unreal.MaterialExpressionTextureSample, -820, -160)
    tex.set_editor_property("texture", _detail_tex("T_Flash"))
    tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)

    # Colour cools as it ages: lerp(white-hot, ember-orange, Age).
    hot = _const3(m, 1.0, 0.85, 0.55, -700, -380)
    ember = _const3(m, 0.95, 0.32, 0.07, -700, -280)
    color = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -500, -320)
    _lib.connect_material_expressions(hot, "", color, "A")
    _lib.connect_material_expressions(ember, "", color, "B")
    _lib.connect_material_expressions(age, "", color, "Alpha")

    bright = _const(m, 9.0, -700, -140)
    e1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -340, -240)
    _lib.connect_material_expressions(color, "", e1, "A")
    _lib.connect_material_expressions(bright, "", e1, "B")
    e2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -180, -200)
    _lib.connect_material_expressions(e1, "", e2, "A")
    _lib.connect_material_expressions(tex, "", e2, "B")     # shape by the sprite RGB
    e3 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -20, -160)
    _lib.connect_material_expressions(e2, "", e3, "A")
    _lib.connect_material_expressions(ageinv, "", e3, "B")  # fade
    _lib.connect_material_property(e3, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    o1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -180, 220)
    _lib.connect_material_expressions(tex, "A", o1, "A")
    _lib.connect_material_expressions(ageinv, "", o1, "B")
    _lib.connect_material_property(o1, "", unreal.MaterialProperty.MP_OPACITY)

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def make_wake():
    """Foam wake the moving frigate churns into the sea — a flat white band, not a
    hard stripe. Opacity is driven by SeaWorldManager's per-vertex alpha (ship
    speed x age fade); an across-width edge falloff softens the long sides and
    world-space turbulence breaks it into churned foam."""
    m = _new_material("M_Wake")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided", True)

    foam = _const3(m, 0.90, 0.94, 0.96, -700, -120)
    _lib.connect_material_property(foam, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    vcolor = _lib.create_material_expression(m, unreal.MaterialExpressionVertexColor, -1000, 120)

    # Across-width edge falloff: 1 - (|U-0.5|*2)^1.6  (soft long edges).
    uv = _lib.create_material_expression(m, unreal.MaterialExpressionTextureCoordinate, -1000, 320)
    umask = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -850, 320)
    umask.set_editor_property("r", True)
    umask.set_editor_property("g", False)
    umask.set_editor_property("b", False)
    umask.set_editor_property("a", False)
    _lib.connect_material_expressions(uv, "", umask, "")
    uhalf = _const(m, 0.5, -850, 450)
    usub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -700, 340)
    _lib.connect_material_expressions(umask, "", usub, "A")
    _lib.connect_material_expressions(uhalf, "", usub, "B")
    uabs = _lib.create_material_expression(m, unreal.MaterialExpressionAbs, -560, 340)
    _lib.connect_material_expressions(usub, "", uabs, "")
    utwo = _const(m, 2.0, -560, 450)
    umul = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -430, 350)
    _lib.connect_material_expressions(uabs, "", umul, "A")
    _lib.connect_material_expressions(utwo, "", umul, "B")
    upow = _lib.create_material_expression(m, unreal.MaterialExpressionPower, -300, 350)
    upow.set_editor_property("const_exponent", 1.6)
    _lib.connect_material_expressions(umul, "", upow, "Base")
    edge = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -170, 350)
    _lib.connect_material_expressions(upow, "", edge, "")

    # World-space turbulence — but SOFT (cut 0.30->0.05, gain 2.0->1.0, finer 0.004->0.006) and
    # FLOORED into [0.6, 1.0] so it gently churns the foam instead of breaking it to ZERO between
    # patches. The old high-contrast mask read as discrete dashes on a slow/distant wake.
    # MULTI-SCALE world-space turbulence so the foam reads as ragged whitewater, not a solid white
    # band: a coarse patch noise x a finer churn noise, floored at 0.30 (ragged holes, but not the
    # zero-gaps that read as dashes on a slow/distant wake).
    n_coarse = _panned_noise(m, 0.006, (0.0, 0.0, 0.0), 0.05, 1.0, ax=-560, ay=640)
    n_fine = _panned_noise(m, 0.022, (1.2, 0.7, 0.0), 0.0, 1.3, ax=-560, ay=920)
    ncomb = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -420, 700)
    _lib.connect_material_expressions(n_coarse, "", ncomb, "A")
    _lib.connect_material_expressions(n_fine, "", ncomb, "B")
    noise = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -280, 700)
    _lib.connect_material_expressions(_const(m, 0.30, -420, 820), "", noise, "A")
    _lib.connect_material_expressions(_const(m, 1.0, -420, 900), "", noise, "B")
    _lib.connect_material_expressions(ncomb, "", noise, "Alpha")

    base_op = _const(m, 1.2, -700, 180)
    o1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -120, 160)
    _lib.connect_material_expressions(base_op, "", o1, "A")
    _lib.connect_material_expressions(vcolor, "A", o1, "B")
    o2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, 40, 220)
    _lib.connect_material_expressions(o1, "", o2, "A")
    _lib.connect_material_expressions(edge, "", o2, "B")
    o3 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, 200, 280)
    _lib.connect_material_expressions(o2, "", o3, "A")
    _lib.connect_material_expressions(noise, "", o3, "B")
    osat = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, 340, 280)
    _lib.connect_material_expressions(o3, "", osat, "")
    _lib.connect_material_property(osat, "", unreal.MaterialProperty.MP_OPACITY)

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    return m


def make_spray():
    """White water-spray puff for the hull waterline ISM particles — small
    aerated droplets thrown off the hull where waves slap it. Camera-facing
    billboard (same ISM / Plane mesh path as M_RocketSmoke). Age (0..1) is
    read from per-instance custom-data slot 0 exactly as make_rocket_smoke()
    does (MaterialExpressionPerInstanceCustomData, data_index=0). Opacity =
    soft round Fresnel feather * (1-Age); emissive = near-white water colour."""
    m = _new_material("M_Spray")
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    m.set_editor_property("two_sided", True)
    m.set_editor_property("used_with_instanced_static_meshes", True)

    # Per-instance Age 0..1 from custom-data slot 0 (same as make_rocket_smoke).
    age = _instance_age(m, -1000, 400)

    # White spray colour — slightly blue-tinted aerated water.
    white = _const3(m, 0.95, 0.97, 1.0, -700, -120)
    _lib.connect_material_property(white, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    # RADIAL UV feather: a camera-facing billboard is flat-on, so Fresnel (≈0 when facing) can't
    # soften it — that left hard white SQUARES. Fade by distance from the UV centre instead, so the
    # plane reads as a soft round droplet (opaque centre -> transparent edge).
    uv = _lib.create_material_expression(m, unreal.MaterialExpressionTextureCoordinate, -900, 200)
    feather = _lib.create_material_expression(m, unreal.MaterialExpressionCustom, -700, 200)
    feather.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    in_uv = unreal.CustomInput(); in_uv.set_editor_property("input_name", "UV")
    feather.set_editor_property("inputs", [in_uv])
    feather.set_editor_property("code", "return saturate(1.0 - 2.1 * length(UV - 0.5));")
    _lib.connect_material_expressions(uv, "", feather, "UV")

    # Age fade: (1 - Age) so the puff starts opaque and fully dissolves.
    ageinv = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -700, 400)
    _lib.connect_material_expressions(age, "", ageinv, "")

    # opacity = feather * (1-Age), then saturate.
    o1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -360, 280)
    _lib.connect_material_expressions(feather, "", o1, "A")
    _lib.connect_material_expressions(ageinv, "", o1, "B")
    osat = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -200, 280)
    _lib.connect_material_expressions(o1, "", osat, "")
    _lib.connect_material_property(osat, "", unreal.MaterialProperty.MP_OPACITY)

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


def assign_by_slot(mesh_name, slot_map, default):
    """Assign a UE material PER FBX material slot, keyed by the imported slot name
    (the Blender material name). Lets one mesh carry several materials — hull paint
    vs. superstructure grey vs. dark sensors/glass — instead of one flat coat."""
    mesh = unreal.load_asset(f"{MESH_DIR}/{mesh_name}")
    if mesh is None:
        raise RuntimeError(f"missing mesh {mesh_name}")
    slots = mesh.static_materials
    for i in range(len(slots)):
        name = str(slots[i].get_editor_property("imported_material_slot_name"))
        # A UE FBX REIMPORT can duplicate slots with a "_<n>" suffix when the mesh
        # sections change (e.g. SM_Frigate -> HullGray, ..., HullGray_1, ...). Match
        # the base name so the extra slots still get the right material instead of
        # silently falling back to `default`.
        base = re.sub(r"_\d+$", "", name)
        mesh.set_material(i, slot_map.get(name) or slot_map.get(base, default))
    unreal.EditorAssetLibrary.save_loaded_asset(mesh)
    unreal.log(f"SeaShieldMaterials: {mesh_name} slots={[str(s.get_editor_property('imported_material_slot_name')) for s in slots]}")


# ============================================================================
# From-scratch realistic ocean (2026-06-18). Replaces the removed SLW/CineOcean
# experiments. A custom dense grid (SM_Ocean) displaced by a 32-wave Gerstner sum
# (WPO), shaded as a Substrate Default-Lit TRANSLUCENT surface-forward material:
# real refraction + Beer-Lambert depth absorption + analytic wave normal + Jacobian
# whitecap foam + Fresnel sky-reflection in EMISSIVE (so grazing is never black) +
# analytic sun glitter. Max quality, fps ignored. Render-only — buoyancy samples the
# hidden plugin ocean. Built by apply_ocean.py on the throwaway L_RangeCustom.
# ============================================================================
_OCN_WIND_DEG = 40.0
_OCN_N = 32
_OCN_LAM_MAX = 18000.0      # cm — 180 m long swell
_OCN_LAM_MIN = 220.0        # cm — 2.2 m chop
_OCN_HS = 320.0             # significant wave height target (cm) — livelier sea
_OCN_STEEP = 0.92           # total horizontal pinch (<1 = crests sharpen but don't loop)
_OCN_G = 981.0              # cm/s^2 (deep-water dispersion omega = sqrt(g*k))
_OCN_FOAM_THRESH = 0.95     # Jacobian fold threshold (J below this -> whitecap)
_OCN_FOAM_SHARP = 3.5
_OCN_CREST_H = 90.0         # cm — crest-height foam assist starts (whitecaps on tall crests)
_OCN_CREST_R = 80.0
_OCN_SHALLOW = (0.06, 0.22, 0.26)   # shallow/near water tint
_OCN_DEEP = (0.004, 0.020, 0.045)   # deep open-ocean body
_OCN_FOAMCOL = (0.96, 0.98, 1.0)
_OCN_DEPTH_RANGE_M = 6.0    # m over which shallow -> deep
_OCN_SKY = (0.18, 0.30, 0.46)       # grazing sky-reflection colour (emissive floor)
_OCN_SKY_STR = 0.9
_OCN_DEBUG_FOAM = False              # DIAG: emissive = mask*red to visualise a foam mask


def _ocean_waves():
    """Deterministic 32-wave ocean spectrum (build-time only — NOT the sim RNG). Geometric
    wavelengths 2.2-180 m, amplitude ~ sqrt(lambda) (long swell dominant) normalised to a ~2.3 m
    significant height, directions jittered ±(tighter for swell, wider for chop) about the wind,
    per-wave steepness Q normalised so the total horizontal pinch stays < 1 (crests don't loop).
    Returns (lambda_cm, amp_cm, dir_x, dir_y, Q, omega) per wave."""
    s = [0x2545F491]

    def rnd():
        s[0] = (s[0] * 1103515245 + 12345) & 0x7fffffff
        return s[0] / 0x7fffffff

    n = _OCN_N
    raw = [(_OCN_LAM_MAX * (_OCN_LAM_MIN / _OCN_LAM_MAX) ** (i / (n - 1))) for i in range(n)]
    weights = [lam ** 0.5 for lam in raw]
    scale = (_OCN_HS * 0.5) / sum(weights)
    waves = []
    for i, lam in enumerate(raw):
        f = i / (n - 1)
        a = weights[i] * scale
        k = 2.0 * math.pi / lam
        omega = math.sqrt(_OCN_G * k)
        ang = math.radians(_OCN_WIND_DEG + (rnd() * 2.0 - 1.0) * (10.0 + 62.0 * f))
        q = (_OCN_STEEP / n) / (k * a)
        waves.append((lam, a, math.cos(ang), math.sin(ang), q, omega))
    return waves


def _ocean_arrays():
    w = _ocean_waves()

    def arr(name, idx, fmt):
        return "float " + name + "[" + str(len(w)) + "] = {" + ",".join(fmt % x[idx] for x in w) + "};"

    return "\n".join([
        "const int N = " + str(len(w)) + ";",
        arr("WL", 0, "%.2f"), arr("AM", 1, "%.4f"),
        arr("DX", 2, "%.6f"), arr("DY", 3, "%.6f"),
        arr("QF", 4, "%.7f"), arr("SP", 5, "%.6f"),
    ])


# Custom-HLSL: sum-of-Gerstner displacement -> WPO (vertex stage).
_OCEAN_DISP_HLSL = _ocean_arrays() + """
float3 P = WP; float t = T;
float3 disp = float3(0.0, 0.0, 0.0);
for (int i = 0; i < N; i++) {
    float2 d = float2(DX[i], DY[i]);
    float k = 6.28318530718 / WL[i];
    float ph = k * dot(d, P.xy) + SP[i] * t;
    float c = cos(ph), s = sin(ph);
    float qa = QF[i] * AM[i];
    disp.x += qa * d.x * c;
    disp.y += qa * d.y * c;
    disp.z += AM[i] * s;
}
return disp;
"""

# Custom-HLSL: analytic Gerstner surface normal (world == tangent on the flat grid) + Jacobian-fold
# whitecap foam, in one float4 (normal.xyz, foam.w). Pixel stage.
_OCEAN_SURF_HLSL = _ocean_arrays() + ("""
float3 P = WP; float t = T;
float nx = 0.0, ny = 0.0, jz = 0.0, h = 0.0;
float Jxx = 0.0, Jyy = 0.0, Jxy = 0.0;
for (int i = 0; i < N; i++) {
    float2 d = float2(DX[i], DY[i]);
    float k = 6.28318530718 / WL[i];
    float ph = k * dot(d, P.xy) + SP[i] * t;
    float c = cos(ph), s = sin(ph);
    float wa = k * AM[i];
    nx += -d.x * wa * c;
    ny += -d.y * wa * c;
    jz += QF[i] * wa * s;
    h  += AM[i] * s;
    Jxx += -QF[i] * d.x * d.x * wa * s;
    Jyy += -QF[i] * d.y * d.y * wa * s;
    Jxy += -QF[i] * d.x * d.y * wa * s;
}
float3 nrm = normalize(float3(nx, ny, 1.0 - jz));
float J = (1.0 + Jxx) * (1.0 + Jyy) - Jxy * Jxy;
float foldFoam = saturate((%(ft)f - J) * %(fs)f);
float crestFoam = saturate((h - %(ch)f) / %(cr)f);
float foam = saturate(foldFoam + crestFoam * 0.8);
return float4(nrm, foam);
""" % {"ft": _OCN_FOAM_THRESH, "fs": _OCN_FOAM_SHARP, "ch": _OCN_CREST_H, "cr": _OCN_CREST_R})


def _ocean_custom(m, code, out_type, ax, ay):
    """A WP(float3 world pos)+T(time) Custom HLSL node — the shared shape for the ocean disp/surf."""
    wp = _lib.create_material_expression(m, unreal.MaterialExpressionWorldPosition, ax - 320, ay)
    tm = _lib.create_material_expression(m, unreal.MaterialExpressionTime, ax - 320, ay + 140)
    cust = _lib.create_material_expression(m, unreal.MaterialExpressionCustom, ax, ay)
    cust.set_editor_property("output_type", out_type)
    in_wp = unreal.CustomInput(); in_wp.set_editor_property("input_name", "WP")
    in_t = unreal.CustomInput(); in_t.set_editor_property("input_name", "T")
    cust.set_editor_property("inputs", [in_wp, in_t])
    cust.set_editor_property("code", code)
    _lib.connect_material_expressions(wp, "", cust, "WP")
    _lib.connect_material_expressions(tm, "", cust, "T")
    return cust


def _ocean_glitter(m, ax, ay):
    """Analytic sun glint: pow(saturate(reflect . sun), hi) gated by sparse panned noise -> HDR
    emissive. Guarded — returns None if a SkyAtmosphere/Reflection node is unavailable headless."""
    try:
        refl = _lib.create_material_expression(m, unreal.MaterialExpressionReflectionVectorWS, ax, ay)
        sun = _lib.create_material_expression(m, unreal.MaterialExpressionSkyAtmosphereLightDirection, ax, ay + 150)
        try:
            sun.set_editor_property("light_index", 0)
        except Exception:  # noqa: BLE001
            pass
        dp = _lib.create_material_expression(m, unreal.MaterialExpressionDotProduct, ax + 200, ay + 60)
        _lib.connect_material_expressions(refl, "", dp, "A")
        _lib.connect_material_expressions(sun, "", dp, "B")
        ds = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, ax + 360, ay + 60)
        _lib.connect_material_expressions(dp, "", ds, "")
        pw = _lib.create_material_expression(m, unreal.MaterialExpressionPower, ax + 520, ay + 60)
        pw.set_editor_property("const_exponent", 160.0)  # broader so the glitter road reads
        _lib.connect_material_expressions(ds, "", pw, "Base")
        gate = _panned_noise(m, 0.02, (1.5, -1.0, 0.0), 0.20, 3.0, ax + 200, ay + 360)
        g1 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 700, ay + 60)
        _lib.connect_material_expressions(pw, "", g1, "A")
        _lib.connect_material_expressions(gate, "", g1, "B")
        g2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 860, ay + 60)
        _lib.connect_material_expressions(g1, "", g2, "A")
        _lib.connect_material_expressions(_const(m, 28.0, ax + 700, ay + 200), "", g2, "B")
        out = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 1020, ay + 60)
        _lib.connect_material_expressions(g2, "", out, "A")
        _lib.connect_material_expressions(_const3(m, 1.0, 0.95, 0.82, ax + 860, ay + 200), "", out, "B")
        return out
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(f"SeaShieldMaterials: ocean glitter skipped ({exc})")
        return None


def _ocean_sss(m, nrm, ax, ay):
    """Fake subsurface scattering — backlit wave crests glow jade-green when the sun is behind the
    wave (the 'lit jade' signature of real ocean). Crytek-style half-vector back-scatter, additive
    emissive. Guarded — returns None if a SkyAtmosphere/Camera node is unavailable headless."""
    try:
        L = _lib.create_material_expression(m, unreal.MaterialExpressionSkyAtmosphereLightDirection, ax, ay)
        try:
            L.set_editor_property("light_index", 0)
        except Exception:  # noqa: BLE001
            pass
        V = _lib.create_material_expression(m, unreal.MaterialExpressionCameraVectorWS, ax, ay + 150)
        cust = _lib.create_material_expression(m, unreal.MaterialExpressionCustom, ax + 240, ay + 60)
        cust.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT1)
        in_l = unreal.CustomInput(); in_l.set_editor_property("input_name", "L")
        in_n = unreal.CustomInput(); in_n.set_editor_property("input_name", "N")
        in_v = unreal.CustomInput(); in_v.set_editor_property("input_name", "V")
        cust.set_editor_property("inputs", [in_l, in_n, in_v])
        cust.set_editor_property("code", "float3 H = normalize(L + N * 0.4);\nreturn pow(saturate(dot(-V, H)), 3.0);")
        _lib.connect_material_expressions(L, "", cust, "L")
        _lib.connect_material_expressions(nrm, "", cust, "N")
        _lib.connect_material_expressions(V, "", cust, "V")
        sc = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 420, ay + 60)
        _lib.connect_material_expressions(cust, "", sc, "A")
        _lib.connect_material_expressions(_const(m, 1.6, ax + 300, ay + 200), "", sc, "B")
        out = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, ax + 560, ay + 60)
        _lib.connect_material_expressions(sc, "", out, "A")
        _lib.connect_material_expressions(_const3(m, 0.05, 0.33, 0.24, ax + 420, ay + 200), "", out, "B")
        return out
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(f"SeaShieldMaterials: ocean SSS skipped ({exc})")
        return None


def make_ocean():
    """The from-scratch realistic ocean material (see section header). Substrate Default-Lit,
    BLEND_TRANSLUCENT, surface-forward — emissive works (unlike SLW), refraction is real, and we
    own the reflective look so grazing is never the SLW black mirror."""
    m = _new_material("M_Ocean")
    m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    try:
        m.set_editor_property("translucency_lighting_mode",
                              unreal.TranslucencyLightingMode.TLM_SURFACE_FORWARD_SHADING)
    except Exception as exc:  # noqa: BLE001
        unreal.log_warning(f"SeaShieldMaterials: TLM set skipped ({exc})")
    try:
        m.set_editor_property("refraction_method", unreal.RefractionMode.RM_PIXEL_NORMAL_OFFSET)
    except Exception:  # noqa: BLE001
        pass

    # displacement (WPO) + analytic normal/foam
    disp = _ocean_custom(m, _OCEAN_DISP_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT3, -1900, -260)
    _lib.connect_material_property(disp, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
    surf = _ocean_custom(m, _OCEAN_SURF_HLSL, unreal.CustomMaterialOutputType.CMOT_FLOAT4, -1900, 260)
    nrm = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -1640, 240)
    nrm.set_editor_property("r", True); nrm.set_editor_property("g", True)
    nrm.set_editor_property("b", True); nrm.set_editor_property("a", False)
    _lib.connect_material_expressions(surf, "", nrm, "")
    _lib.connect_material_property(nrm, "", unreal.MaterialProperty.MP_NORMAL)
    foam_raw = _lib.create_material_expression(m, unreal.MaterialExpressionComponentMask, -1640, 380)
    foam_raw.set_editor_property("r", False); foam_raw.set_editor_property("g", False)
    foam_raw.set_editor_property("b", False); foam_raw.set_editor_property("a", True)
    _lib.connect_material_expressions(surf, "", foam_raw, "")
    # INTERSECTION FOAM — the wash where the water meets the HULL. The translucent water sees the
    # opaque hull behind it (SceneDepth); where SceneDepth ~ PixelDepth the water is right against the
    # hull -> white foam collar. Near-gated (no foam at the far mesh->FarOcean seam). It's in the ocean
    # material, so it rides the visible waves AND hugs the moving hull automatically.
    sd_e = _lib.create_material_expression(m, unreal.MaterialExpressionSceneDepth, -1640, 560)
    pd_e = _lib.create_material_expression(m, unreal.MaterialExpressionPixelDepth, -1640, 700)
    ediff = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -1480, 600)
    _lib.connect_material_expressions(sd_e, "", ediff, "A")
    _lib.connect_material_expressions(pd_e, "", ediff, "B")
    edn = _lib.create_material_expression(m, unreal.MaterialExpressionDivide, -1340, 600)
    _lib.connect_material_expressions(ediff, "", edn, "A")
    _lib.connect_material_expressions(_const(m, 700.0, -1480, 660), "", edn, "B")   # ~7 m wash band at the hull
    eone = _lib.create_material_expression(m, unreal.MaterialExpressionOneMinus, -1200, 600)
    _lib.connect_material_expressions(edn, "", eone, "")
    edge = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -1080, 600)
    _lib.connect_material_expressions(eone, "", edge, "")
    ngn = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -1340, 780)
    _lib.connect_material_expressions(_const(m, 42000.0, -1480, 780), "", ngn, "A")  # fade out beyond ~420 m
    _lib.connect_material_expressions(pd_e, "", ngn, "B")
    ngd = _lib.create_material_expression(m, unreal.MaterialExpressionDivide, -1200, 780)
    _lib.connect_material_expressions(ngn, "", ngd, "A")
    _lib.connect_material_expressions(_const(m, 16000.0, -1340, 860), "", ngd, "B")
    ngs = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -1080, 780)
    _lib.connect_material_expressions(ngd, "", ngs, "")
    edge_g = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -940, 680)
    _lib.connect_material_expressions(edge, "", edge_g, "A")
    _lib.connect_material_expressions(ngs, "", edge_g, "B")
    foam_in = _lib.create_material_expression(m, unreal.MaterialExpressionMax, -800, 460)
    _lib.connect_material_expressions(foam_raw, "", foam_in, "A")
    _lib.connect_material_expressions(edge_g, "", foam_in, "B")
    # break the foam into ragged whitewater patches (world-space panned turbulence) instead of smooth
    # crest tips — the difference between 'highlights' and 'foam'
    fnoise = _panned_noise(m, 0.03, (2.0, 1.5, 0.0), 0.0, 1.0, -1500, 520)
    fbk = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -1340, 460)
    _lib.connect_material_expressions(_const(m, 0.25, -1500, 440), "", fbk, "A")
    _lib.connect_material_expressions(_const(m, 1.3, -1500, 600), "", fbk, "B")
    _lib.connect_material_expressions(fnoise, "", fbk, "Alpha")
    fmul = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -1200, 400)
    _lib.connect_material_expressions(foam_in, "", fmul, "A")
    _lib.connect_material_expressions(fbk, "", fmul, "B")
    fsat = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -1060, 400)
    _lib.connect_material_expressions(fmul, "", fsat, "")
    # CONTRAST: crush the low-mid foam wash so only strong foam whitens -> crisp scattered whitecaps
    fsub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -920, 400)
    _lib.connect_material_expressions(fsat, "", fsub, "A")
    _lib.connect_material_expressions(_const(m, 0.45, -1060, 520), "", fsub, "B")
    fcon = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -780, 400)
    _lib.connect_material_expressions(fsub, "", fcon, "A")
    _lib.connect_material_expressions(_const(m, 4.0, -920, 520), "", fcon, "B")
    foam_w = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -640, 400)
    _lib.connect_material_expressions(fcon, "", foam_w, "")
    # the hull-intersection collar bypasses the wave-foam contrast (stays a strong wash) but gets a
    # light noise break so it reads as organic whitewater hugging the hull, not a clean ring.
    enb = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -620, 520)
    _lib.connect_material_expressions(_const(m, 0.5, -760, 520), "", enb, "A")
    _lib.connect_material_expressions(_const(m, 1.1, -760, 600), "", enb, "B")
    _lib.connect_material_expressions(fnoise, "", enb, "Alpha")
    edge_b = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -560, 460)
    _lib.connect_material_expressions(edge_g, "", edge_b, "A")
    _lib.connect_material_expressions(enb, "", edge_b, "B")
    foam = _lib.create_material_expression(m, unreal.MaterialExpressionMax, -420, 440)
    _lib.connect_material_expressions(foam_w, "", foam, "A")
    _lib.connect_material_expressions(edge_b, "", foam, "B")

    # Fresnel from the wave pixel-normal (drives sky reflection + colour blend)
    fres = _lib.create_material_expression(m, unreal.MaterialExpressionFresnel, -1200, 720)
    fres.set_editor_property("exponent", 5.0)
    try:
        fres.set_editor_property("base_reflect_fraction", 0.02)
    except Exception:  # noqa: BLE001
        pass

    # depth (Beer-Lambert): waterDepth = SceneDepth - PixelDepth  -> deep colour with depth
    sd = _lib.create_material_expression(m, unreal.MaterialExpressionSceneDepth, -1200, 0)
    pd = _lib.create_material_expression(m, unreal.MaterialExpressionPixelDepth, -1200, 120)
    wsub = _lib.create_material_expression(m, unreal.MaterialExpressionSubtract, -1040, 40)
    _lib.connect_material_expressions(sd, "", wsub, "A")
    _lib.connect_material_expressions(pd, "", wsub, "B")
    wmax = _lib.create_material_expression(m, unreal.MaterialExpressionMax, -900, 40)
    _lib.connect_material_expressions(wsub, "", wmax, "A")
    _lib.connect_material_expressions(_const(m, 0.0, -1040, 160), "", wmax, "B")
    wdm = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -760, 40)  # cm->m
    _lib.connect_material_expressions(wmax, "", wdm, "A")
    _lib.connect_material_expressions(_const(m, 0.01, -900, 160), "", wdm, "B")
    dfrac = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -620, 40)
    _lib.connect_material_expressions(wdm, "", dfrac, "A")
    _lib.connect_material_expressions(_const(m, 1.0 / _OCN_DEPTH_RANGE_M, -760, 160), "", dfrac, "B")
    depthFade = _lib.create_material_expression(m, unreal.MaterialExpressionSaturate, -480, 40)
    _lib.connect_material_expressions(dfrac, "", depthFade, "")

    # base colour: shallow -> deep by depth, -> foam white
    body = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -300, 0)
    _lib.connect_material_expressions(_const3(m, *_OCN_SHALLOW, -480, -120), "", body, "A")
    _lib.connect_material_expressions(_const3(m, *_OCN_DEEP, -480, -40), "", body, "B")
    _lib.connect_material_expressions(depthFade, "", body, "Alpha")
    base = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -140, -40)
    _lib.connect_material_expressions(body, "", base, "A")
    _lib.connect_material_expressions(_const3(m, *_OCN_FOAMCOL, -300, 140), "", base, "B")
    _lib.connect_material_expressions(foam, "", base, "Alpha")
    _lib.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)

    # opacity: see-through near hull (shallow) -> opaque deep; foam fully opaque
    op0 = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -300, 300)
    _lib.connect_material_expressions(_const(m, 0.5, -480, 280), "", op0, "A")
    _lib.connect_material_expressions(_const(m, 0.97, -480, 360), "", op0, "B")
    _lib.connect_material_expressions(depthFade, "", op0, "Alpha")
    op = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -140, 300)
    _lib.connect_material_expressions(op0, "", op, "A")
    _lib.connect_material_expressions(_const(m, 1.0, -300, 440), "", op, "B")
    _lib.connect_material_expressions(foam, "", op, "Alpha")
    _lib.connect_material_property(op, "", unreal.MaterialProperty.MP_OPACITY)

    # roughness (low = crisp reflection) -> foam matte; specular; metallic; refraction
    rough = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -300, 560)
    _lib.connect_material_expressions(_const(m, 0.03, -480, 540), "", rough, "A")
    _lib.connect_material_expressions(_const(m, 0.85, -480, 620), "", rough, "B")
    _lib.connect_material_expressions(foam, "", rough, "Alpha")
    _lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    spec = _lib.create_material_expression(m, unreal.MaterialExpressionLinearInterpolate, -300, 700)
    _lib.connect_material_expressions(_const(m, 0.5, -480, 680), "", spec, "A")
    _lib.connect_material_expressions(_const(m, 0.3, -480, 760), "", spec, "B")
    _lib.connect_material_expressions(foam, "", spec, "Alpha")
    _lib.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)
    _lib.connect_material_property(_const(m, 0.0, -300, 820), "", unreal.MaterialProperty.MP_METALLIC)
    _lib.connect_material_property(_const(m, 1.05, -300, 880), "", unreal.MaterialProperty.MP_REFRACTION)

    # emissive: Fresnel sky-reflection floor (never black) + sun glitter + foam glow
    skyrefl = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -900, 900)
    _lib.connect_material_expressions(_const3(m, *_OCN_SKY, -1060, 880), "", skyrefl, "A")
    skf = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -1060, 1000)
    _lib.connect_material_expressions(fres, "", skf, "A")
    _lib.connect_material_expressions(_const(m, _OCN_SKY_STR, -1200, 1020), "", skf, "B")
    _lib.connect_material_expressions(skf, "", skyrefl, "B")
    foamglow = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -900, 1100)
    _lib.connect_material_expressions(_const3(m, *_OCN_FOAMCOL, -1060, 1080), "", foamglow, "A")
    fg2 = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -1060, 1180)
    _lib.connect_material_expressions(foam, "", fg2, "A")
    _lib.connect_material_expressions(_const(m, 0.22, -1200, 1200), "", fg2, "B")
    _lib.connect_material_expressions(fg2, "", foamglow, "B")
    emis = _lib.create_material_expression(m, unreal.MaterialExpressionAdd, -700, 980)
    _lib.connect_material_expressions(skyrefl, "", emis, "A")
    _lib.connect_material_expressions(foamglow, "", emis, "B")
    glit = _ocean_glitter(m, -1100, 1320)
    if glit is not None:
        emis2 = _lib.create_material_expression(m, unreal.MaterialExpressionAdd, -540, 1000)
        _lib.connect_material_expressions(emis, "", emis2, "A")
        _lib.connect_material_expressions(glit, "", emis2, "B")
        emis = emis2
    sss = _ocean_sss(m, nrm, -1100, 1560)
    if sss is not None:
        emis3 = _lib.create_material_expression(m, unreal.MaterialExpressionAdd, -380, 1020)
        _lib.connect_material_expressions(emis, "", emis3, "A")
        _lib.connect_material_expressions(sss, "", emis3, "B")
        emis = emis3
    _lib.connect_material_property(emis, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    if _OCN_DEBUG_FOAM:  # DIAG: show the foam mask directly (red = foam coverage/magnitude)
        dbg = _lib.create_material_expression(m, unreal.MaterialExpressionMultiply, -540, 1500)
        _lib.connect_material_expressions(foam, "", dbg, "A")  # DIAG: the final foam mask
        _lib.connect_material_expressions(_const3(m, 4.0, 0.0, 0.0, -700, 1500), "", dbg, "B")
        _lib.connect_material_property(dbg, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    _lib.recompile_material(m)
    unreal.EditorAssetLibrary.save_loaded_asset(m)
    unreal.log("SeaShieldMaterials: M_Ocean built (from-scratch translucent-forward Gerstner ocean)")
    return m


def main():
    hull = make_naval_hull()
    # P3-7b — naval-AD: the superstructure (this material backs both DeckDark + Superstructure)
    # read "glass-smooth" because a 480 cm plate tile shows <1 plate on a ~3-6 m deckhouse face.
    # Tighten to 220 cm so the re-authored crisp plate/weld seams actually land on the
    # superstructure + deck, sharing one plating language with the hull (the #1 mid-distance
    # greybox tell). The macro dirt modulation already breaks the tighter tile's repeat.
    # P3-7c 3-TIER VALUE SPLIT (naval-AD: "the ship is one flat gray albedo; a warship is never
    # one value"). The superstructure was DARKER than the hull (inverted). Now: superstructure +
    # decks = LIGHTEST haze-gray, hull = MID (make_naval_hull haze 0.10), sensors/radar/mast-array
    # = DARKEST near-black. The value separation is what the eye reads first at distance.
    # P3-7f — naval-AD 3: the 3-tier ORDER was right but the LIGHT-vs-MID gap (0.14 vs hull 0.10)
    # was too small to survive the bright-sky exposure — it read as TWO tiers. Push the
    # superstructure/deck UP to ~0.20 and cool it slightly (blue highest) for a >=0.08 absolute
    # separation from the 0.10 hull, so the lighter haze-gray topside reads as a distinct tier.
    deck_gray = make_detailed("M_NavalGray", (0.190, 0.205, 0.220), 0.6, 0.15, tile_cm=400.0)
    # P3-7/Phase-7 (naval-AD re-judge, B-): SensorDark "still vanishes into the gray — nothing
    # reads as black radar glass". Push it to genuine black glass: darker base + a LOW roughness
    # floor so the SPY/AESA faces catch a sharp specular streak under the now-raking key, with
    # metallic for a tinted dark mirror. This is the dominant remaining "putty monolith" tell.
    sensor_dark = make_detailed("M_SensorDark", (0.020, 0.024, 0.032), 0.12, 0.6, tile_cm=150.0)
    # P3-7/Phase-7.2 (naval-AD re-judge): the armament (gun house/CIWS mounts) read as
    # "hull-colored extrusions" — weapons need their own MATERIAL FAMILY. A dark, glossy,
    # metallic "machinery metal" tier sits BETWEEN the light hull gray (0.19) and the
    # black-glass sensors (0.02): weapons now separate as bare gunmetal, not paint.
    gunmetal = make_detailed("M_Gunmetal", (0.090, 0.100, 0.110), 0.30, 0.75, tile_cm=150.0)
    missile_white = make_detailed("M_MissileBody", (0.45, 0.45, 0.43), 0.38, 0.2, tile_cm=170.0)
    rocket_olive = make_detailed("M_RocketBody", (0.105, 0.115, 0.060), 0.55, 0.1, tile_cm=170.0)
    make_trail()
    make_rocket_smoke()
    make_splash()
    make_burst()
    make_muzzle()
    make_debris()
    make_wake()
    make_rain()
    make_far_ocean()
    make_sea_ocean()

    assign_by_slot("SM_Frigate",
                   {"HullGray": hull, "DeckDark": deck_gray, "Superstructure": deck_gray,
                    "SensorDark": sensor_dark, "Gunmetal": gunmetal}, hull)
    assign("SM_LauncherBase", deck_gray)
    assign("SM_LauncherMount", deck_gray)
    assign("SM_LauncherTubes", deck_gray)
    assign("SM_Missile", missile_white)
    assign("SM_Rocket", rocket_olive)
    unreal.log("SeaShieldMaterials: 16 materials created, 6 meshes assigned")


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
