#include "physics3d/Physics3D.h"

#include "core/Providers.h"

#include <functional>

// The physics3d interface's shared bits: names, Settings, and the backend
// a Trading Post plugin provides (see core/Providers.h). The Jolt backend
// lives in src/plugins/physics_jolt.

namespace ts {
namespace tombstone {
namespace physics3d {

const char* default_layer_name(int layer) {
  static const char* names[kLayerCount] = {"Static", "Default", "Held",     "Player",
                                           "Debris", "Trigger", "Custom A", "Custom B"};
  return names[layer & 7];
}
const char* motion_name(Motion m) {
  switch (m) {
    case Motion::Static: return "static";
    case Motion::Kinematic: return "kinematic";
    case Motion::Dynamic: return "dynamic";
  }
  return "static";
}
const char* shape_name(ShapeType s) {
  switch (s) {
    case ShapeType::Box: return "box";
    case ShapeType::Sphere: return "sphere";
    case ShapeType::Capsule: return "capsule";
    case ShapeType::ConvexHull: return "convex";
    case ShapeType::Mesh: return "mesh";
    case ShapeType::HeightField: return "heightfield";
  }
  return "box";
}
const char* joint_name(JointType t) {
  switch (t) {
    case JointType::Fixed: return "fixed";
    case JointType::Hinge: return "hinge";
    case JointType::Slider: return "slider";
    case JointType::Distance: return "distance";
    case JointType::Ball: return "ball";
  }
  return "fixed";
}

Settings::Settings() {
  collide.fill(0xff);
  // Debris only hits the world and other debris by default.
  for (int i = 0; i < kLayerCount; ++i) {
    if (i != kLayerStatic && i != kLayerDebris) set_collides(kLayerDebris, i, false);
  }
}

void Settings::set_collides(int a, int b, bool on) {
  a &= 7;
  b &= 7;
  if (on) {
    collide[a] |= static_cast<std::uint8_t>(1u << b);
    collide[b] |= static_cast<std::uint8_t>(1u << a);
  } else {
    collide[a] &= static_cast<std::uint8_t>(~(1u << b));
    collide[b] &= static_cast<std::uint8_t>(~(1u << a));
  }
}

using Factory = WorldFactory;

std::unique_ptr<World> create_world(const Settings& settings, std::string* backend) {
  std::string id;
  const Factory make = core::Providers<Factory>::instance().current(&id);
  if (backend) *backend = make ? id : std::string();
  if (!make) return nullptr;
  return make(settings);
}

std::string active_backend() {
  std::string id;
  return core::Providers<Factory>::instance().current(&id) ? id : std::string();
}

}  // namespace physics3d
}  // namespace tombstone
}  // namespace ts
