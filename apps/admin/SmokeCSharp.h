#pragma once

// C# scripts: with the .NET 8 SDK, builds a project's scripts/*.cs, rides
// hooks / props / state / the plugin bridge, hot-reloads (state kept, old
// AssemblyLoadContext unloaded), reports a compile error with file:line,
// swaps a background rebuild in, mixes Lua + graph + C# (+ C++) in one
// world, and exports a game that runs the shipped assembly. Without the
// SDK it checks the graceful disable (clear Telegraph hint, the rest of the
// world rides on) and reports SKIPPED; TS_SMOKE_REQUIRE_CS=1 makes that a
// failure.
int run_csharp_smoke();
