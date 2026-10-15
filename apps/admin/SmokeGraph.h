#pragma once

// Visual scripting checks: graph parse / write round trip, catalog checks,
// interpreter (events, flow, math, variables, entity / UI nodes), delays,
// buttons, budget containment, hot reload, determinism across two runs,
// and a mixed Lua + graph project.
int run_graph_smoke();

#include <memory>
namespace ts::tombstone::runtime { struct WorldExtensions; }
std::shared_ptr<const ts::tombstone::runtime::WorldExtensions> smoke_plugin_extensions();
