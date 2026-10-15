#pragma once

// Visual scripting checks: graph parse / write round trip, catalog checks,
// interpreter (events, flow, math, variables, entity / UI nodes), delays,
// buttons, budget containment, hot reload, determinism across two runs,
// and a mixed Lua + graph project.
int run_graph_smoke();
