#pragma once

namespace ts {
namespace tombstone {

// ts_admin --smoke-update-check (also part of --smoke): the update
// notifier with canned GitHub JSON and a loopback server; no internet.
int run_update_smoke();

}  // namespace tombstone
}  // namespace ts
