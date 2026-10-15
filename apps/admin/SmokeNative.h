#pragma once

// C++ gameplay scripts: generate + build the per-project module, ride it
// (hooks, props, entity state, toast), hot reload with state kept, a
// compile error reported to the Telegraph with the old module kept.
// SKIPPED (exit 0) when no cmake / compiler; TS_SMOKE_REQUIRE_CPP=1 fails.
int run_native_smoke();
