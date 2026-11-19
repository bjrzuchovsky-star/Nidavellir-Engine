#pragma once

// Headless --smoke checks for 3D projects: math + primitives, scene.json
// v8 round trip (and 2D / 3D refusing each other's files), the
// ghost-town-3d sample + glTF loading, the 3D play runtime (rider vs
// walls, ground, jump, triggers, Lua 3D methods), a GL 3.3 offscreen
// render with pixel checks (sky, lit vs shadowed ground, point light) when
// a display is available, and the 3D editor (create, place, gizmo drags
// with snap, undo / redo, pick, drag-drop a .gltf, Play / Stop). 0 = pass.
int run_3d_smoke();

// --smoke-dump: write the frame and the shadow map under smoke-dump/ and
// print the shadow depth range. Checked by --smoke-3d.
void set_smoke_dump(bool on);

// --smoke-present: a few frames into the real swapchain, then read the
// backbuffer back. Direct3D 12 and Vulkan. 0 = pass.
int run_present_smoke();

// --smoke-realm: create a 3D realm, render a few frames, close. Direct3D 12
// with --gfx-debug fails if the debug layer stored an error. 0 = pass.
int run_realm_smoke();

// --smoke-window: the editor's real visible window (the path --open and the
// project manager use). Hidden smokes do not cover it. 0 = pass.
int run_window_smoke();

// --smoke-switch: Project Manager frames, then a double-click into Editor3D
// on the same device (no relaunch), then more frames. Direct3D 12 with
// --gfx-debug fails if the debug layer stored an error. 0 = pass.
int run_switch_smoke();
