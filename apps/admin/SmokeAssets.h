#pragma once

// Headless --smoke-assets: the asset database, importers and the Asset
// Browser's editor operations (2D + 3D). .meta files with stable GUIDs,
// PNG / glTF / WAV / font import info, import settings, renames and moves
// in the editor and outside it (with and without the .meta), copies,
// references kept working, find references, delete-in-use refusal, hot
// reload of changed files, settings applied on drop. 0 = pass.
int run_assets_smoke();
