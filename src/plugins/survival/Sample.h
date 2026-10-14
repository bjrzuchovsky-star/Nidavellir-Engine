#pragma once

// The "frontier-survival" sample project: a 2D claim with Frontier Survival
// switched on, one rider, a following camera, a scripted supply crate
// (frontier.give from Lua) and the default data files under survival/.

#include <string>

namespace ts {
namespace tombstone {
namespace survival {

inline constexpr const char* kSampleId = "frontier-survival";
inline constexpr const char* kSampleName = "Frontier Survival";
inline constexpr const char* kCrateScript = "scripts/supply_crate.lua";

// Writes project.json, scene.json, the rider sheet, the crate script and
// survival/*.json into `dir`. Existing files are kept (never overwritten).
bool write_frontier_sample(const std::string& dir, const std::string& name,
                           std::string* error);

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
