#pragma once

// Headless --smoke checks for plugins and project types: load order,
// dependencies (missing / disabled / wrong version / loops / refusals),
// static + dynamic registration, plugin systems and Lua tables in a World,
// enable / disable persistence in project.json, the editor's restart-of-
// play semantics and the posse-as-plugin, 2D / 3D / VR round trips, old
// project.json files, and the 3D / VR survey camp. 0 when all pass.
int run_plugin_smoke();
