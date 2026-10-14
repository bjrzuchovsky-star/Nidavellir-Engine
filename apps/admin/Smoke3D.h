#pragma once

// Headless --smoke checks for 3D projects: math + primitives, scene.json
// v7 round trip (and 2D / 3D refusing each other's files), the
// ghost-town-3d sample + glTF loading, the 3D play runtime (rider vs
// walls, ground, jump, triggers, Lua 3D methods), a GL 3.3 offscreen
// render with pixel checks (sky, lit vs shadowed ground, point light) when
// a display is available, and the 3D editor (create, place, gizmo drags
// with snap, undo / redo, pick, drag-drop a .gltf, Play / Stop). 0 = pass.
int run_3d_smoke();
