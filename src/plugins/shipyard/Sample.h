#pragma once

// The "sky-pirates" sample project: a 2D claim with Airship Works on, one
// rider at the helm of the Dust Devil sloop, a pair of pirate marauders, a
// Lua bounty board (the `ships` table) and the stock parts / designs under
// shipyard/.

#include <string>

namespace ts {
namespace tombstone {
namespace shipyard {

inline constexpr const char* kSampleId = "sky-pirates";
inline constexpr const char* kSampleName = "Sky Pirates";
inline constexpr const char* kBountyScript = "scripts/bounty_board.lua";
inline constexpr const char* kSloopPath = "shipyard/designs/sloop.ship.json";
inline constexpr const char* kMarauderPath = "shipyard/designs/marauder.ship.json";

// Writes project.json, scene.json, the bounty script and shipyard/* into
// `dir`. Existing files are kept (never overwritten).
bool write_sky_pirates_sample(const std::string& dir, const std::string& name, std::string* error);

}  // namespace shipyard
}  // namespace tombstone
}  // namespace ts
