#pragma once

// ts.ai from C++ scripts: thin wrappers over the plugin bridge (the same
// `ai` Lua table scripts and graphs use). Needs the ts.ai plugin on.
//
//   #include <tombstone/ai.h>
//   void on_start() override { ts::ai::agent(*this, self(), 120); ts::ai::bt(*this, self(), "ai/guard.bt.json"); }
//
// 2D positions are world px (x, y); 3D ones metres (x, y up, z): use the *3 forms.

#include <tombstone/script_sdk.h>

#include <string>

namespace ts {
namespace ai {

inline double num_(const PropValue& v) { return v.type == 'x' ? 0.0 : v.number; }

// Make an entity an agent (2D: speed px/s; 3D uses the Agent component or defaults).
inline bool agent(Script& s, Entity e, double speed = 0.0) {
  return num_(s.plugin("ai", "agent", {arg(static_cast<double>(e)), arg(speed)})) != 0.0;
}
inline bool bt(Script& s, Entity e, const std::string& path) {
  return num_(s.plugin("ai", "bt", {arg(static_cast<double>(e)), arg(path)})) != 0.0;
}
inline bool move_to(Script& s, Entity e, double x, double y) {
  return num_(s.plugin("ai", "move_to", {arg(static_cast<double>(e)), arg(x), arg(y)})) != 0.0;
}
inline bool move_to3(Script& s, Entity e, double x, double y, double z) {
  return num_(s.plugin("ai", "move_to", {arg(static_cast<double>(e)), arg(x), arg(y), arg(z)})) != 0.0;
}
inline void stop(Script& s, Entity e) { s.plugin("ai", "stop", {arg(static_cast<double>(e))}); }
inline bool moving(Script& s, Entity e) { return num_(s.plugin("ai", "moving", {arg(static_cast<double>(e))})) != 0.0; }
// Walkable path length, -1 when there is none.
inline double path_length(Script& s, double x0, double y0, double x1, double y1) {
  const PropValue v = s.plugin("ai", "path_length", {arg(x0), arg(y0), arg(x1), arg(y1)});
  return v.type == 'x' ? -1.0 : v.number;
}
inline double path_length3(Script& s, double x0, double y0, double z0, double x1, double y1, double z1) {
  const PropValue v = s.plugin("ai", "path_length", {arg(x0), arg(y0), arg(z0), arg(x1), arg(y1), arg(z1)});
  return v.type == 'x' ? -1.0 : v.number;
}
// Running branch of the behaviour tree ("selector > patrol > move_to").
inline std::string state(Script& s, Entity e) { return s.plugin("ai", "state", {arg(static_cast<double>(e))}).text; }
inline void set_bb(Script& s, Entity e, const std::string& key, double v) {
  s.plugin("ai", "set_bb", {arg(static_cast<double>(e)), arg(key), arg(v)});
}
inline void set_bb(Script& s, Entity e, const std::string& key, const std::string& v) {
  s.plugin("ai", "set_bb", {arg(static_cast<double>(e)), arg(key), arg(v)});
}
inline double get_bb(Script& s, Entity e, const std::string& key) {
  return num_(s.plugin("ai", "get_bb", {arg(static_cast<double>(e)), arg(key)}));
}
inline void noise(Script& s, double x, double y, double loudness = 1.0) {
  s.plugin("ai", "noise", {arg(x), arg(y), arg(loudness)});
}
inline void noise3(Script& s, double x, double y, double z, double loudness = 1.0) {
  s.plugin("ai", "noise", {arg(x), arg(y), arg(z), arg(loudness)});
}
inline bool can_see(Script& s, Entity e, Entity target) {
  return num_(s.plugin("ai", "can_see", {arg(static_cast<double>(e)), arg(static_cast<double>(target))})) != 0.0;
}
inline int count(Script& s) { return static_cast<int>(num_(s.plugin("ai", "count"))); }

}  // namespace ai
}  // namespace ts
