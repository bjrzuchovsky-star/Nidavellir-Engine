#pragma once

// --smoke-loc: string tables, tr() in Lua / graph / C ABI, UI text keys,
// the pause-menu language button and font fallback.
int run_loc_smoke();
// --smoke-tiles: 47-blob / Wang 16 / rule autotiles, the terrain brush and
// the sprite sheet slicer (grid, auto-detect, clips, .anim.json).
int run_tiles_smoke();
