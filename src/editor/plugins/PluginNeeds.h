#pragma once

// Physics / movement plugins (0.2.0 split) the open scene uses, from the
// editor's in-memory scene. Same rules as plugins::required_plugins (which
// reads the files): see plugins/split/Split.h.

#include "render3d/Scene3D.h"
#include "scene/SceneData.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

std::vector<std::string> plugin_needs(const scene3d::Scene3D& scene);
std::vector<std::string> plugin_needs(const std::vector<Entity2D>& entities);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
