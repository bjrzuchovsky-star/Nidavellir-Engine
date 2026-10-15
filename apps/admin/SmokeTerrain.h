#pragma once

// --smoke-terrain: heightfield brushes, LOD chunks, .tsterrain round trip,
// Scene3D v16, the Jolt heightfield (raycast + a rider standing on a hill),
// foliage paint / erase determinism, null-renderer foliage counts and the
// Lua `terrain` table.
int run_terrain_smoke();
// --smoke-weather: the deterministic Sim (transitions, cycle, lightning,
// time of day), weather.json, the 2D plugin's quads / ambient, the 3D
// environment + billboards, particle wind and the Lua `weather` table.
int run_weather_smoke();
