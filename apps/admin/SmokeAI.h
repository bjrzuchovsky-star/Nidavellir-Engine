#pragma once

// Headless checks for the AI toolkit (ts_ai + the ts.ai plugin): grid A*
// optimality against Dijkstra, JPS / HPA* agreement, flow fields, Recast
// navmesh build + paths + off-mesh links, crowd steering without overlap,
// behaviour-tree semantics + determinism, utility scoring, perception, the
// state machine, and the plugin in 2D / Frontier / 3D worlds. 0 = pass.
int run_ai_smoke();
