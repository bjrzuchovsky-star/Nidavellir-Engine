#include "ai/Perception.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace ai {

bool in_sight_cone(const V3& eye, const V3& forward, const V3& target, float range, float fov_deg) {
  const V3 d = target - eye;
  const float dist = length(d);
  if (dist > range) return false;
  if (dist < 1e-4f) return true;
  const V3 f = normalized(flat(forward));
  const V3 dir = normalized(flat(d));
  if (length(f) < 0.5f || length(dir) < 0.5f) return true;
  const float half = fov_deg * 0.5f * 3.14159265f / 180.0f;
  return dot(f, dir) >= std::cos(half) - 1e-5f;
}

Percept& Perception::slot(std::uint64_t id) {
  if (id != 0) {
    for (Percept& p : known_) {
      if (p.id == id) return p;
    }
  }
  known_.push_back(Percept{});
  known_.back().id = id;
  return known_.back();
}

void Perception::see(const V3& eye, const V3& forward, const std::vector<Candidate>& candidates,
                     const LineClearFn& clear, double now) {
  for (Percept& p : known_) p.visible = false;
  for (const Candidate& c : candidates) {
    const float dist = distance(eye, c.pos);
    const bool close = dist <= config.near_sense;
    if (!close && !in_sight_cone(eye, forward, c.pos, config.sight_range, config.fov_deg)) continue;
    if (clear && !clear(eye, c.pos)) continue;
    Percept& p = slot(c.id);
    p.pos = c.pos;
    p.seen = now;
    p.visible = true;
  }
}

bool Perception::hear(std::uint64_t source, const V3& at, float loudness, const std::string& what,
                      const V3& listener, double now) {
  if (loudness <= 0.0f || distance(at, listener) > config.hearing_range * loudness) return false;
  Percept& p = slot(source);
  if (!p.visible) p.pos = at;
  p.heard = now;
  p.sound = what;
  p.loudness = loudness;
  return true;
}

void Perception::forget(double now) {
  known_.erase(std::remove_if(known_.begin(), known_.end(),
                              [&](const Percept& p) {
                                const double last = std::max(p.seen, p.heard);
                                return !p.visible && now - last > config.memory;
                              }),
               known_.end());
}

const Percept* Perception::find(std::uint64_t id) const {
  for (const Percept& p : known_) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

bool Perception::sees(std::uint64_t id) const {
  const Percept* p = find(id);
  return p && p->visible;
}

bool Perception::sees_any() const {
  return std::any_of(known_.begin(), known_.end(), [](const Percept& p) { return p.visible; });
}

const Percept* Perception::best(const V3& from) const {
  const Percept* out = nullptr;
  for (const Percept& p : known_) {
    if (!out) {
      out = &p;
      continue;
    }
    if (p.visible != out->visible) {
      if (p.visible) out = &p;
      continue;
    }
    if (p.visible) {
      if (distance(from, p.pos) < distance(from, out->pos)) out = &p;
    } else if (std::max(p.seen, p.heard) > std::max(out->seen, out->heard)) {
      out = &p;
    }
  }
  return out;
}

const Percept* Perception::last_heard(double now, double window) const {
  const Percept* out = nullptr;
  for (const Percept& p : known_) {
    if (p.heard < 0.0 || now - p.heard > window) continue;
    if (!out || p.heard > out->heard) out = &p;
  }
  return out;
}

}  // namespace ai
}  // namespace ts
