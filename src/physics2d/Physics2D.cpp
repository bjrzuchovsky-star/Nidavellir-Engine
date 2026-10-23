#include "physics2d/Physics2D.h"

#include "core/Providers.h"

#include <functional>

// The 2D physics backend a Trading Post plugin provides (see
// core/Providers.h). The Box2D backend lives in src/plugins/physics_box2d.

namespace ts {
namespace tombstone {
namespace physics2d {

using Factory = WorldFactory;

std::unique_ptr<World> create_world(Vec2 gravity, float pixels_per_metre, std::string* backend) {
  std::string id;
  const Factory make = core::Providers<Factory>::instance().current(&id);
  if (backend) *backend = make ? id : std::string();
  if (!make) return nullptr;
  return make(gravity, pixels_per_metre);
}

std::string active_backend() {
  std::string id;
  return core::Providers<Factory>::instance().current(&id) ? id : std::string();
}

}  // namespace physics2d
}  // namespace tombstone
}  // namespace ts
