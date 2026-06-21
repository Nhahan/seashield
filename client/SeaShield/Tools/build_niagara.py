#!/usr/bin/env python3
"""Reproducible builder for SeaShield's water-VFX Niagara systems (NS_Spray, NS_Wake).

SOURCE OF TRUTH for the Niagara assets: drives the Monolith MCP server (HTTP JSON-RPC on
127.0.0.1:9316, hosted by a running UnrealEditor with the Monolith authoring plugin) to build
the systems declaratively, so the binary .uassets are regenerable from this script — the same
discipline as setup_materials.py (materials) / bpy (meshes). Monolith is an AUTHORING-TIME tool
only (gitignored, not vendored, not part of the game runtime).

The C++ runtime (SeaWorldManager) spawns these as UNiagaraComponents on the ownship and drives
them per-frame via the User.* parameters. create_system_from_spec OVERWRITES, so this is idempotent.

Run:  (editor with Monolith up, "listening on port 9316")  ->  python3 Tools/build_niagara.py
"""
import json
import urllib.request

URL = "http://127.0.0.1:9316/mcp"

# ---- engine module-script paths (resolved on UE5.8 via list_module_scripts) ----
MINIMAL = "/Niagara/DefaultAssets/Templates/Emitters/Minimal"   # bare emitter: EmitterState + InitializeParticle + 1 sprite renderer
M_SPAWN = "/Niagara/Modules/Emitter/SpawnRate.SpawnRate"
M_SHAPE = "/Niagara/Modules/Spawn/Location/V2/ShapeLocation.ShapeLocation"
M_CONE  = "/Niagara/Modules/Spawn/Velocity/AddVelocityInCone.AddVelocityInCone"
M_GRAV  = "/Niagara/Modules/Update/Forces/GravityForce.GravityForce"
M_CURL  = "/Niagara/Modules/Update/Forces/CurlNoiseForce.CurlNoiseForce"
M_SOLVE = "/Niagara/Modules/Solvers/SolveForcesAndVelocity.SolveForcesAndVelocity"
MAT_SPRAY = "/Game/SeaShield/Materials/M_SpraySprite"   # lit-volumetric sprite (setup_materials.py)
MAT_WAKE  = "/Game/SeaShield/Materials/M_WakeRibbon"    # sun-driven foam ribbon (setup_materials.py)


def _call(tool, args):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                       "params": {"name": tool, "arguments": args}}).encode()
    req = urllib.request.Request(URL, body, {
        "Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
    d = json.loads(urllib.request.urlopen(req, timeout=600).read().decode())  # first-time shader/Niagara compile can block the game-thread HTTP tick for minutes
    if "error" in d:
        return {"_isError": True, "_rpc": d["error"]}
    res = d.get("result", {})
    c = res.get("content", [])
    txt = c[0]["text"] if c and "text" in c[0] else ""
    try:
        out = json.loads(txt)
    except Exception:
        out = {"_text": txt}
    if res.get("isError"):
        out["_isError"] = True
    return out


def nia(action, **p):
    return _call("niagara_query", {"action": action, "params": p})


def _ok(label, r):
    bad = isinstance(r, dict) and r.get("_isError")
    print(("  ✗ " if bad else "  · ") + label + ("  " + json.dumps(r)[:150] if bad else ""))
    return not bad


def setv(P, E, mod, inp, val):
    return _ok(f"{mod}.{inp}={val!r}", nia("set_module_input_value", asset_path=P, emitter=E,
                                            module_node=mod, input=inp, value=val))


def setsw(P, E, mod, inp, val):
    return _ok(f"{mod}[{inp}]={val!r}", nia("set_static_switch_value", asset_path=P, emitter=E,
                                            module_node=mod, input=inp, value=val))


def bindp(P, E, mod, inp, param):
    return _ok(f"{mod}.{inp}<-{param}", nia("set_module_input_binding", asset_path=P, emitter=E,
                                            module_node=mod, input=inp, binding=param))


def ribbon_index(P, E):
    r = nia("list_renderers", asset_path=P, emitter=E)
    for rr in r.get("renderers", []):
        if rr.get("type") == "ribbon":
            return rr["index"]
    return -1


def sprite_index(P, E):
    r = nia("list_renderers", asset_path=P, emitter=E)
    for rr in r.get("renderers", []):
        if rr.get("type") == "sprite":
            return rr["index"]
    return -1


# ============================ NS_Spray (lit sprite spray) ============================
NS_SPRAY = "/Game/SeaShield/VFX/NS_Spray"
SPRAY_SPEC = {
    "user_parameters": [{"name": "SprayRate", "type": "float", "default": 12.0}],  # small ambient baseline; the C++ runtime drives it up with hull speed via User.SprayRate
    "emitters": [{
        "asset": MINIMAL, "name": "Spray", "properties": {"SimTarget": "CPU"},
        "modules": [
            {"stage": "emitter_update",  "script": M_SPAWN},
            {"stage": "particle_spawn",  "script": M_SHAPE},
            {"stage": "particle_spawn",  "script": M_CONE},
            {"stage": "particle_update", "script": M_GRAV},
            {"stage": "particle_update", "script": M_CURL},
            {"stage": "particle_update", "script": M_SOLVE},
        ],
        # Minimal already provides a sprite renderer — don't add a second.
    }]
}


def build_spray():
    print("=== NS_Spray ===")
    _ok("create", nia("create_system_from_spec", save_path=NS_SPRAY, spec=SPRAY_SPEC))
    P, E = NS_SPRAY, "Spray"
    setsw(P, E, "InitializeParticle", "Lifetime Mode", "Direct Set")
    setv(P, E, "InitializeParticle", "Lifetime", 0.7)
    setsw(P, E, "InitializeParticle", "Sprite Size Mode", "Uniform")
    setv(P, E, "InitializeParticle", "Uniform Sprite Size", 55.0)
    # spawn in a THIN DISC at the waterline around the hull footprint (component is attached at
    # waterline Z=0) — concentrates spray at the waterline instead of scattering up over the deck.
    setsw(P, E, "ShapeLocation", "Shape Primitive", "Cylinder")
    setv(P, E, "ShapeLocation", "Cylinder Radius", 6000.0)
    setv(P, E, "ShapeLocation", "Cylinder Height", 300.0)
    bindp(P, E, "SpawnRate", "SpawnRate", "User.SprayRate")
    setv(P, E, "AddVelocityInCone", "Cone Angle", 42.0)
    setv(P, E, "AddVelocityInCone", "Velocity Strength", 460.0)
    setv(P, E, "AddVelocityInCone", "Cone Axis", "0,0,1")
    setv(P, E, "CurlNoiseForce", "Noise Strength", 260.0)
    setv(P, E, "CurlNoiseForce", "Noise Frequency", 6.0)
    # lit-volumetric material (kills the default white-disc look)
    si = sprite_index(P, E)
    if si >= 0:
        _ok(f"sprite[{si}] material<-M_SpraySprite",
            nia("set_renderer_material", asset_path=P, emitter=E, renderer_index=si, material=MAT_SPRAY))
    nia("request_compile", asset_path=P, force=True)
    print("  diag:", json.dumps(nia("get_system_diagnostics", asset_path=P))[:220])
    _ok("save", nia("save_system", asset_path=P))


# ============================ NS_Wake (foam ribbon trail) ============================
NS_WAKE = "/Game/SeaShield/VFX/NS_Wake"
WAKE_SPEC = {
    "user_parameters": [{"name": "WakeRate", "type": "float", "default": 50.0}],  # TEMP nonzero for render test; runtime drives it
    "emitters": [{
        "asset": MINIMAL, "name": "Wake", "properties": {"SimTarget": "CPU"},
        "modules": [{"stage": "emitter_update", "script": M_SPAWN}],
        "renderers": [{"class": "NiagaraRibbonRendererProperties"}],
    }]
}


def build_wake():
    print("=== NS_Wake ===")
    _ok("create", nia("create_system_from_spec", save_path=NS_WAKE, spec=WAKE_SPEC))
    P, E = NS_WAKE, "Wake"
    # drop Minimal's sprite renderer — we only want the ribbon
    si = sprite_index(P, E)
    if si >= 0:
        _ok(f"remove sprite renderer[{si}]", nia("remove_renderer", asset_path=P, emitter=E, renderer_index=si))
    ri = ribbon_index(P, E)
    # KEY FIX: Minimal never writes Particles.RibbonLinkOrder (stays 0 -> ribbon can't order
    # itself -> renders nothing). Bind link order to NormalizedAge so particles string into a
    # trail from the ship (new) to the tail (old).
    _ok(f"ribbon[{ri}] RibbonLinkOrder<-NormalizedAge",
        nia("set_renderer_binding", asset_path=P, emitter=E, renderer_index=ri,
            binding_name="RibbonLinkOrderBinding", attribute="Particles.NormalizedAge"))
    # long-lived, wide, world-space ribbon points (trail behind the steaming ship)
    setsw(P, E, "InitializeParticle", "Lifetime Mode", "Direct Set")
    setv(P, E, "InitializeParticle", "Lifetime", 6.0)
    setsw(P, E, "InitializeParticle", "Ribbon Width Mode", "Direct Set")
    setv(P, E, "InitializeParticle", "Ribbon Width", 700.0)
    bindp(P, E, "SpawnRate", "SpawnRate", "User.WakeRate")
    _ok(f"ribbon[{ri}] material<-M_WakeRibbon",
        nia("set_renderer_material", asset_path=P, emitter=E, renderer_index=ri, material=MAT_WAKE))
    nia("request_compile", asset_path=P, force=True)
    print("  diag:", json.dumps(nia("get_system_diagnostics", asset_path=P))[:220])
    _ok("save", nia("save_system", asset_path=P))


if __name__ == "__main__":
    build_spray()
    # build_wake()  # NS_Wake ribbon is unused in the hybrid (legacy mesh wake covers the wake);
    # kept as a function for reference / future full-Niagara revisit.
