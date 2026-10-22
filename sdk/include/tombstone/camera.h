#pragma once

// Cameras from C++ scripts: thin wrappers over the plugin bridge (the same
// `camera` Lua table scripts and graphs use). See docs/camera.md.
//
//   #include <tombstone/camera.h>
//   void on_interact(Entity rider) override { ts::camera::set_active(*this, "Cellar Camera"); }

#include <tombstone/script_sdk.h>

#include <string>

namespace ts {
namespace camera {

inline double num_(const PropValue& v) { return v.type == 'n' || v.type == 'b' ? v.number : 0.0; }

// Look through this camera now (until reset or another set_active).
inline bool set_active(Script& s, Entity cam) {
  return num_(s.plugin("camera", "set_active", {arg(static_cast<double>(cam))})) != 0.0;
}
inline bool set_active(Script& s, const std::string& name) {
  return num_(s.plugin("camera", "set_active", {arg(name)})) != 0.0;
}
// Back to the highest-priority active camera.
inline void reset(Script& s) { s.plugin("camera", "reset", {}); }
// Camera Play looks through (0 = the built-in follow view).
inline Entity active(Script& s) { return static_cast<Entity>(num_(s.plugin("camera", "active", {}))); }
inline bool set_priority(Script& s, Entity cam, int priority) {
  return num_(s.plugin("camera", "set_priority", {arg(static_cast<double>(cam)), arg(static_cast<double>(priority))})) != 0.0;
}

}  // namespace camera
}  // namespace ts
