#pragma once

// The 2D rider controllers of the Top-down Rider (ts.move.topdown) and
// Platformer Rider (ts.move.platformer) Trading Post plugins. offer_2d_riders()
// makes them available (runtime::RiderControllers2D); the plugins switch
// them on per claim.

namespace ts {
namespace tombstone {
namespace plugins {

void offer_2d_riders();

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
