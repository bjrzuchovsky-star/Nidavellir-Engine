#pragma once

#include "plugins/split/Split.h"

// The ambient hold main() takes for smoke runs (every physics / movement
// provider on). --smoke-plugin-split clears it to ride without plugins.
ts::tombstone::plugins::ProviderHold& smoke_provider_hold();

namespace ts {
namespace tombstone {

// ts_admin --smoke-plugin-split (also part of --smoke): the 0.2.0 split of
// physics and rider movement into Trading Post plugins. Templates switch on
// what they need, a pre-0.2.0 claim migrates once (backup + log), claims
// ride with and without the plugins, and an export ships only what the
// claim uses.
int run_plugin_split_smoke();

}  // namespace tombstone
}  // namespace ts
