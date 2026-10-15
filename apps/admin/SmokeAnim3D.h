#pragma once

// Headless checks for 3D skeletal animation: skin math against a hand
// reference, clip sampling (linear / step / cubic spline, slerp), 1D blend
// weights, the generated Drifter + hand glTFs through cgltf, the animator
// (blend space by speed, crossfade, jump trigger + landing, upper-body
// layer mask, events, root motion), scene.json v12 round trip, World3D +
// Lua / graph anim API, and a GPU skinned render pixel check. 0 = pass.
int run_anim3d_smoke();
