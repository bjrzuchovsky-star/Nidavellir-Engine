#pragma once

namespace ts {
namespace tombstone {

// ts_admin --smoke-team (also part of --smoke): Live Team Claims. A team
// server and headless 2D / 3D editors in one process over loopback ENet:
// conflicting edits converge, soft locks turn edits back, a crashed
// editor's locks are released, late joiners catch up from snapshot + tail,
// the version guard refuses mismatched scene formats.
int run_team_smoke();

}  // namespace tombstone
}  // namespace ts
