# Offscreen capture of L_Range — the inspect half of the visual iteration
# loop (same discipline as the Blender preview renders in tools/assets).
# Uses a SceneCapture2D + render target rather than the editor viewport: the
# viewport never renders under -unattended, so viewport screenshots come out
# black. Run like setup_level.py:
#   UnrealEditor SeaShield.uproject -unattended \
#     -ExecCmds="py client/SeaShield/Tools/capture_level.py"
import os

import unreal

LEVEL = "/Game/SeaShield/Maps/L_Range"
OUT_DIR = os.path.join(unreal.SystemLibrary.get_project_directory(), "Saved", "Screenshots", "MacEditor")
OUT_NAME = "SeaShieldCapture.png"
# Quarter view of the frigate at the stage origin (SeaWorldFrame::Origin =
# 3 km NE of world zero — see SeaWorldFrame.h): 175 m out, 40 m up.
CAMERA_LOCATION = unreal.Vector(285000.0, 291000.0, 4000.0)
CAMERA_ROTATION = unreal.Rotator(0.0, -13.0, 31.0)  # roll, pitch, yaw
WARMUP_SECONDS = 45.0  # shader compilation / streaming (water materials are heavy)
ADAPT_CAPTURES = 30  # repeated captures let eye adaptation converge

_state = {"phase": "load", "seconds": 0.0, "captures": 0, "handle": None}


def _finish(message, error=False):
    unreal.unregister_slate_post_tick_callback(_state["handle"])
    (unreal.log_error if error else unreal.log)(message)
    unreal.SystemLibrary.quit_editor()


def _setup_capture():
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    target = unreal.RenderingLibrary.create_render_target2d(
        world, 1920, 1080, unreal.TextureRenderTargetFormat.RTF_RGBA8_SRGB
    )
    actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
        unreal.SceneCapture2D, CAMERA_LOCATION, CAMERA_ROTATION
    )
    component = actor.capture_component2d
    component.texture_target = target
    component.capture_source = unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR
    component.capture_every_frame = False
    _state["world"] = world
    _state["target"] = target
    _state["component"] = component


def _tick(delta_seconds):
    _state["seconds"] += delta_seconds
    try:
        if _state["phase"] == "load":
            if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(LEVEL):
                _finish(f"SeaShieldCapture: failed to load {LEVEL}", error=True)
                return
            _state["phase"] = "warm"
            _state["seconds"] = 0.0
        elif _state["phase"] == "warm":
            if _state["seconds"] >= WARMUP_SECONDS:
                _setup_capture()
                _state["phase"] = "adapt"
        elif _state["phase"] == "adapt":
            _state["component"].capture_scene()
            _state["captures"] += 1
            if _state["captures"] >= ADAPT_CAPTURES:
                os.makedirs(OUT_DIR, exist_ok=True)
                unreal.RenderingLibrary.export_render_target(
                    _state["world"], _state["target"], OUT_DIR, OUT_NAME
                )
                out_path = os.path.join(OUT_DIR, OUT_NAME)
                if os.path.isfile(out_path):
                    _finish(f"SeaShieldCapture: saved {out_path}")
                else:
                    _finish("SeaShieldCapture: export produced no file", error=True)
    except Exception:  # noqa: BLE001
        import traceback

        _finish(f"SeaShieldCapture: FAILED\n{traceback.format_exc()}", error=True)


if __name__ == "__main__":
    _state["handle"] = unreal.register_slate_post_tick_callback(_tick)
