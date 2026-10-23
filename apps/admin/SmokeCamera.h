#pragma once

// Headless --smoke-camera: cameras as scene objects in 2D and 3D
// (docs/camera.md). Create / select / move / parent a camera, every
// inspector field round-trips, the highest-priority active camera plays,
// scripts switch cameras (Lua, graph, the bridge C++ / C# use), follow
// the rider, 3D boom collision with and without the physics plugin, align
// camera to view, look through, the preview render, VR keeping the HMD
// view, undo, and the old-claim migration (backup, log, same view). 0 = pass.
int run_camera_smoke();

// Headless --smoke-attach: the attach tools (docs/transforms.md "Attach"):
// attach keep / snap, detach keeping the world transform, follow position /
// rotation / both, attach to an animated bone, snap to object, copy
// transform, align to surface, the eyedropper, multi-select, one undo step
// each. 0 = pass.
int run_attach_smoke();
