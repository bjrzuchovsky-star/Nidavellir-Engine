#pragma once

// The Rider 3D controller (ts.move.rider3d) and the 3D offers. Hosts that
// run World3D (ts_admin, ts_game) call offer_3d_providers() once at start-up;
// the Trading Post plugins then switch the parts on per claim.

namespace ts {
namespace tombstone {
namespace plugins {

// Offers the Rider 3D controller (world3d::RiderControllers3D) and the Jolt
// physics backend (physics3d::create_world). Idempotent.
void offer_3d_providers();

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
