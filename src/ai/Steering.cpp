#include "ai/Steering.h"

#include <algorithm>
#include <limits>

namespace ts {
namespace ai {

V3 seek(const SteerAgent& a, const V3& target) {
  return normalized(flat(target - a.pos)) * a.max_speed;
}

V3 arrive(const SteerAgent& a, const V3& target, float slow_radius) {
  const V3 d = flat(target - a.pos);
  const float dist = length(d);
  if (dist < 1e-4f) return {};
  const float speed = dist < slow_radius ? a.max_speed * dist / slow_radius : a.max_speed;
  return d * (speed / dist);
}

V3 separation(const SteerAgent& a, const std::vector<SteerAgent>& others, std::size_t self, float margin) {
  V3 push;
  for (std::size_t i = 0; i < others.size(); ++i) {
    if (i == self) continue;
    const V3 d = flat(a.pos - others[i].pos);
    const float dist = length(d);
    const float reach = a.radius + others[i].radius + margin;
    if (dist >= reach) continue;
    const V3 dir = dist > 1e-4f ? d * (1.0f / dist) : V3{(self < i) ? -1.0f : 1.0f, 0.0f, 0.0f};
    push += dir * ((reach - dist) / reach * a.max_speed);
  }
  return push;
}

V3 avoidance(const SteerAgent& a, const std::vector<SteerAgent>& others, std::size_t self, float horizon) {
  V3 dodge;
  for (std::size_t i = 0; i < others.size(); ++i) {
    if (i == self) continue;
    const SteerAgent& b = others[i];
    const V3 rp = flat(b.pos - a.pos);
    const V3 rv = flat(b.vel - a.vel);
    const float rv2 = dot(rv, rv);
    if (rv2 < 1e-6f) continue;
    const float t = std::clamp(-dot(rp, rv) / rv2, 0.0f, horizon);
    const V3 closest = rp + rv * t;
    const float r = a.radius + b.radius + 0.1f;
    const float miss = length(closest);
    if (miss >= r) continue;
    // Steer away from the closest approach; consistent side by index so two
    // agents pick opposite sides.
    V3 away = closest * -1.0f;
    if (length(away) < 1e-3f) away = V3{-rv.z, 0.0f, rv.x} * (self < i ? 1.0f : -1.0f);
    dodge += normalized(away) * (a.max_speed * (1.0f - t / (horizon + 1e-3f)) * (r - miss) / r * 2.0f);
  }
  return dodge;
}

std::size_t SteerCrowd::add(const SteerAgent& a) {
  agents_.push_back(a);
  desired_.push_back({});
  return agents_.size() - 1;
}

void SteerCrowd::step(float dt) {
  const std::vector<SteerAgent> snap = agents_;
  for (std::size_t i = 0; i < agents_.size(); ++i) {
    SteerAgent& a = agents_[i];
    V3 want = desired_[i];
    if (a.avoid) want += separation(snap[i], snap, i, 0.15f) + avoidance(snap[i], snap, i, 1.5f);
    want = truncate(flat(want), a.max_speed);
    const V3 accel = truncate(want - a.vel, a.max_accel * dt);
    a.vel = truncate(a.vel + accel, a.max_speed);
    a.pos += a.vel * dt;
  }
  // Positional resolve: push overlapping pairs apart (a few passes).
  for (int pass = 0; pass < 4; ++pass) {
    for (std::size_t i = 0; i < agents_.size(); ++i) {
      for (std::size_t j = i + 1; j < agents_.size(); ++j) {
        SteerAgent& a = agents_[i];
        SteerAgent& b = agents_[j];
        V3 d = flat(b.pos - a.pos);
        float dist = length(d);
        const float r = a.radius + b.radius;
        if (dist >= r) continue;
        if (dist < 1e-4f) {
          d = {1.0f, 0.0f, 0.0f};
          dist = 1e-4f;
        }
        const V3 n = d * (1.0f / dist);
        const float push = (r - dist) * 0.5f + 1e-4f;
        a.pos = a.pos - n * push;
        b.pos = b.pos + n * push;
      }
    }
  }
}

float SteerCrowd::min_clearance() const {
  float best = std::numeric_limits<float>::infinity();
  for (std::size_t i = 0; i < agents_.size(); ++i) {
    for (std::size_t j = i + 1; j < agents_.size(); ++j) {
      best = std::min(best, distance_xz(agents_[i].pos, agents_[j].pos) - agents_[i].radius - agents_[j].radius);
    }
  }
  return best;
}

}  // namespace ai
}  // namespace ts
