#pragma once

// Headless --smoke-meta: the claim's metadata folder (.tombstone/meta),
// legacy sidecar migration, relinks, orphans, sorted imports and the Vault
// with both layouts (see SmokeMeta.cpp). 0 = pass.
int run_meta_smoke();

#include <string>

// A demo claim for ts_admin --assets-shot: downloads sorted into type
// folders plus one legacy sidecar asset. Returns the claim folder.
std::string make_sorted_demo_claim(const std::string& root_dir);
