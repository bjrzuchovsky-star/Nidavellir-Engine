#pragma once

// VR smoke (ts_admin --smoke-vr; also part of the console --smoke): scene v8
// vr_rig / grabbable round trip, the Haunted Saloon VR sample driven through
// World3D's VR input (locomotion, snap turn, teleport, grab / drop / throw,
// force grab, laser trigger, comfort menu, Lua `vr` API) and the editor's
// simulator preview. Headless; the GL stereo pixel check is in ts_game --smoke.
int run_vr_smoke();
