#pragma once

// Steering for agents that do not ride a navmesh crowd (2D worlds, simple
// 3D movers): seek / arrive / wander plus neighbour separation and a
// velocity-obstacle style avoidance. All on the XZ plane.

#include "ai/AiMath.h"

#include <cstdint>
#include <vector>

namespace ts {
namespace ai {

struct SteerAgent {
  V3 pos;
  V3 vel;
  float radius = 0.4f;
  float max_speed = 3.0f;
  float max_accel = 12.0f;
  bool avoid = true;
};

V3 seek(const SteerAgent& a, const V3& target);
// Slows down within `slow_radius` and stops at the target.
V3 arrive(const SteerAgent& a, const V3& target, float slow_radius);
// Push away from neighbours closer than their combined radii + margin.
V3 separation(const SteerAgent& a, const std::vector<SteerAgent>& others, std::size_t self, float margin);
// Sideways dodge from neighbours on a collision course within `horizon` s.
V3 avoidance(const SteerAgent& a, const std::vector<SteerAgent>& others, std::size_t self, float horizon);

// A simple crowd: each agent has a desired velocity (from a path / flow
// field); step() blends in separation + avoidance, clamps, integrates and
// finally resolves remaining overlaps (so agents never interpenetrate).
class SteerCrowd {
 public:
  std::size_t add(const SteerAgent& a);
  void set_desired(std::size_t i, const V3& v) { desired_[i] = v; }
  void step(float dt);
  std::vector<SteerAgent>& agents() { return agents_; }
  const std::vector<SteerAgent>& agents() const { return agents_; }
  // Smallest centre distance minus combined radii over all pairs (>= 0
  // means nobody overlaps).
  float min_clearance() const;

 private:
  std::vector<SteerAgent> agents_;
  std::vector<V3> desired_;
};

}  // namespace ai
}  // namespace ts
