#pragma once

// Headless checks for Frontier Survival (ts.frontier): worldgen
// determinism (prints a hash to compare across machines), chunk
// persistence, saddlebags + crafting (and the Lua table), combat, and
// posse replication of tile edits / inventory over ENet loopback with
// plugins on both ts_server's and ts_client's side. 0 when all pass.
int run_survival_smoke();
