#include "editor/plugins/PluginNeeds.h"

#include "plugins/split/Split.h"

#include <set>

namespace ts {
namespace tombstone {
namespace editor {

namespace {
std::vector<std::string> ordered(const std::set<std::string>& need) {
  std::vector<std::string> out;
  for (const std::string& id : plugins::split_plugin_ids()) {
    if (need.count(id)) out.push_back(id);
  }
  return out;
}
}  // namespace

std::vector<std::string> plugin_needs(const scene3d::Scene3D& scene) {
  std::set<std::string> need;
  for (const scene3d::Entity3D& e : scene.entities) {
    if (e.player || e.vr_rig) {
      need.insert(plugins::kRider3DId);
      need.insert(plugins::kJoltId);
    }
    if (e.rigid_body || e.grabbable || e.joint) need.insert(plugins::kJoltId);
  }
  return ordered(need);
}

std::vector<std::string> plugin_needs(const std::vector<Entity2D>& entities) {
  std::set<std::string> need;
  for (const Entity2D& e : entities) {
    if (e.player) {
      need.insert(e.player->mode == PlayerControllerData::Mode::Platformer ? plugins::kPlatformerId
                                                                            : plugins::kTopDownId);
    }
    if (e.body || e.joint) need.insert(plugins::kBox2DId);
  }
  return ordered(need);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
