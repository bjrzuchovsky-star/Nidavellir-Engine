#pragma once

// Headless --smoke-prefab: prefabs in the 2D and 3D editors. Save a
// selection as a .prefab.json, place instances, per-instance overrides,
// asset edits (API and on-disk) updating every instance, revert / apply,
// undo / redo (asset file included), duplicate splitting, save / reload.
// 0 = pass.
int run_prefab_smoke();
