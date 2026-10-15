#pragma once

// Senses for agents: a sight cone (range, field of view, line of sight
// through a host callback) and hearing (sound events within range scaled
// by loudness), with a short memory of where things were last noticed.
// Positions are 3D (y up); 2D worlds pass (x, 0, y).

#include "ai/AiMath.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace ai {

struct PerceptionConfig {
  float sight_range = 12.0f;
  float fov_deg = 120.0f;       // full cone angle
  float near_sense = 1.5f;      // always noticed this close (behind too)
  float hearing_range = 18.0f;  // a loudness 1 sound is heard this far
  float memory = 6.0f;          // seconds a percept is kept after it ends
};

struct Percept {
  std::uint64_t id = 0;     // entity (0 = a sound with no source)
  V3 pos;                   // last known position
  double seen = -1.0;       // time last seen (-1 never)
  double heard = -1.0;      // time last heard
  bool visible = false;     // in sight this update
  std::string sound;        // last sound name
  float loudness = 0.0f;
};

using LineClearFn = std::function<bool(const V3& from, const V3& to)>;

bool in_sight_cone(const V3& eye, const V3& forward, const V3& target, float range, float fov_deg);

class Perception {
 public:
  PerceptionConfig config;

  struct Candidate {
    std::uint64_t id;
    V3 pos;
  };
  // One sight pass: who is in the cone with a clear line (null = always).
  void see(const V3& eye, const V3& forward, const std::vector<Candidate>& candidates,
           const LineClearFn& clear, double now);
  // A sound at `at` (loudness 1 = normal). True when heard.
  bool hear(std::uint64_t source, const V3& at, float loudness, const std::string& what, const V3& listener,
            double now);
  void forget(double now);

  const std::vector<Percept>& known() const { return known_; }
  const Percept* find(std::uint64_t id) const;
  bool sees(std::uint64_t id) const;
  bool sees_any() const;
  // Most relevant: visible ones by distance, then the freshest memory.
  const Percept* best(const V3& from) const;
  // Freshest sound within `window` seconds of now.
  const Percept* last_heard(double now, double window) const;
  void clear() { known_.clear(); }

 private:
  Percept& slot(std::uint64_t id);
  std::vector<Percept> known_;
};

}  // namespace ai
}  // namespace ts
