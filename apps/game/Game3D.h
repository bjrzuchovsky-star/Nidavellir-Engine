#pragma once

// ts_game for "dimension": "3d" projects: GL 3.3 core window over
// world3d::World3D, render3d::SceneView and the follow camera.

#include <string>

namespace ts {
namespace tombstone {
namespace game {

// True when <dir>/scene.json is a 3D scene (or project.json says 3d and
// there is no scene yet).
bool is_3d_project(const std::string& dir);

// Headless check: load, ride 120 ticks, verify, then one offscreen GL
// frame with a pixel check (skipped without a display unless
// TS_SMOKE_REQUIRE_GL is set).
int run_smoke_3d(const std::string& dir, const std::string& name);

// The window; frames < 0 = until closed.
int run_window_3d(const std::string& dir, const std::string& name, long frames);

// ---- "dimension": "vr" (GameVR.cpp) ----------------------------------------
bool is_vr_project(const std::string& dir);
// Headless: the scene in VR mode through the simulator: stereo pixel check
// (with a display), walk, snap turn, grab + throw, teleport.
int run_smoke_vr(const std::string& dir, const std::string& name);
// OpenXR headset (else the simulator unless `simulator`), with a desktop
// mirror window of both eyes.
int run_window_vr(const std::string& dir, const std::string& name, long frames, bool simulator);
// Non-interactive OpenXR check: loader, runtime, headset, session create
// (+ destroy). 0 when a session could be created.
int run_xr_probe();
// Is an OpenXR runtime with a headset there right now? (no window, no GL)
bool headset_present(std::string* why);

}  // namespace game
}  // namespace tombstone
}  // namespace ts
