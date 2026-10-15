#pragma once

// Physics from C++ scripts: thin wrappers over the plugin bridge (the same
// `physics` Lua table scripts and graphs use). 2D worlds: px, px / s,
// degrees (z ignored); 3D worlds: metres, m / s, y up.
//
//   #include <tombstone/physics.h>
//   void on_interact(Entity rider) override { ts::physics::impulse(*this, self(), 0, -400); }

#include <tombstone/script_sdk.h>

#include <string>

namespace ts {
namespace physics {

inline double num_(const PropValue& v) { return v.type == 'n' || v.type == 'b' ? v.number : 0.0; }
inline PropValue e_(Entity e) { return arg(static_cast<double>(e)); }

inline bool force(Script& s, Entity e, double x, double y, double z = 0.0) {
  return num_(s.plugin("physics", "apply_force", {e_(e), arg(x), arg(y), arg(z)})) != 0.0;
}
inline bool impulse(Script& s, Entity e, double x, double y, double z = 0.0) {
  return num_(s.plugin("physics", "apply_impulse", {e_(e), arg(x), arg(y), arg(z)})) != 0.0;
}
inline bool torque(Script& s, Entity e, double x, double y = 0.0, double z = 0.0) {
  return num_(s.plugin("physics", "apply_torque", {e_(e), arg(x), arg(y), arg(z)})) != 0.0;
}
inline bool set_velocity(Script& s, Entity e, double x, double y, double z = 0.0) {
  return num_(s.plugin("physics", "set_velocity", {e_(e), arg(x), arg(y), arg(z)})) != 0.0;
}
// axis 0 = x, 1 = y, 2 = z
inline double velocity(Script& s, Entity e, int axis) {
  return num_(s.plugin("physics", "velocity_axis", {e_(e), arg(static_cast<double>(axis))}));
}
inline double speed(Script& s, Entity e) { return num_(s.plugin("physics", "speed", {e_(e)})); }
// First entity the ray hits (0 = none). 2D: pass z = dz = 0.
inline Entity raycast(Script& s, double x, double y, double z, double dx, double dy, double dz, double dist) {
  const PropValue v = s.plugin("physics", "raycast_xyz", {arg(x), arg(y), arg(z), arg(dx), arg(dy), arg(dz), arg(dist)});
  return v.type == 'n' && v.number > 0 ? static_cast<Entity>(v.number) : 0;
}
// Distance to the first hit, -1 for none.
inline double ray_distance(Script& s, double x, double y, double z, double dx, double dy, double dz, double dist) {
  const PropValue v =
      s.plugin("physics", "ray_distance_xyz", {arg(x), arg(y), arg(z), arg(dx), arg(dy), arg(dz), arg(dist)});
  return v.type == 'n' ? v.number : -1.0;
}
// Join b to a (0 = the world). 2D types weld / revolute / prismatic /
// distance; 3D fixed / hinge / slider / distance / ball. Anchor relative to
// b. Returns the joint handle (0 = failed).
inline double joint(Script& s, Entity a, Entity b, const std::string& type, double ax = 0, double ay = 0,
                    double az = 0) {
  const PropValue v = s.plugin("physics", "joint_xyz", {e_(a), e_(b), arg(type), arg(ax), arg(ay), arg(az)});
  return v.type == 'n' ? v.number : 0.0;
}
inline void unjoint(Script& s, double joint) { s.plugin("physics", "unjoint", {arg(joint)}); }

}  // namespace physics
}  // namespace ts
