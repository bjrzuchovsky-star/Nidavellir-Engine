#pragma once

// Town Hall world rules shared by the server and the client (ts_mmo): the
// persistent world's settings (<project>/mmo.json, optional), its zones and
// their geometry, the one movement step both sides run (so an honest
// client's prediction matches the server exactly), and the interest grid.
//
// mmo.json (every key optional):
//   {"tick_rate": 20, "speed": 160, "interest_radius": 640,
//    "save_every": 30, "linkdead_seconds": 30, "max_players": 512,
//    "open_registration": true, "argon2_kib": 4096,
//    "chat": {"channels": ["trade", "help"], "say_radius": 480,
//             "max_length": 200, "per_10s": 8},
//    "zones": [{"id": "town", "name": "Ghost Town", "scene": "scene.json",
//               "bounds": [x0, y0, x1, y1], "spawn": [x, y],
//               "blockers": [[x0, y0, x1, y1]],
//               "portals": [{"rect": [x0, y0, x1, y1], "to": "mine", "at": [x, y]}],
//               "items": [{"name": "gold_nugget", "at": [x, y], "respawn": 30}]}]}
//
// Without mmo.json the project's scene.json is one zone, "main": bounds
// around its entities, spawn at its first Player, every entity named
// "item:<name>" a pickup. Solid static colliders block riders.

#include "runtime/Collision.h"
#include "scene/SceneData.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace mmo {

inline constexpr float kRiderHalf = 12.0f;  // riders are 24 x 24 boxes, (x, y) = centre

struct Portal {
  runtime::Aabb rect;
  std::string to;
  float at_x = 0.0f, at_y = 0.0f;
};

struct ItemSpawn {
  std::string name;
  float x = 0.0f, y = 0.0f;
  double respawn = 30.0;
};

// What a client needs to predict movement in a zone (sent on entry).
struct ZoneGeometry {
  std::string id;
  std::string name;
  runtime::Aabb bounds{-2048, -2048, 2048, 2048};
  std::vector<runtime::Aabb> blockers;
  std::vector<Portal> portals;
};

struct ZoneDef {
  ZoneGeometry geo;
  std::string scene;  // project-relative scene file ("" = none)
  float spawn_x = 0.0f, spawn_y = 0.0f;
  std::vector<ItemSpawn> items;
  std::vector<Entity2D> entities;  // the scene, for the zone's World
  TileSolidity solidity;
};

struct ChatConfig {
  std::vector<std::string> channels{"trade", "help"};
  float say_radius = 480.0f;
  std::size_t max_length = 200;
  int per_10s = 8;
};

struct WorldConfig {
  int tick_rate = 20;
  float speed = 160.0f;
  float interest_radius = 640.0f;
  double save_every = 30.0;
  double linkdead_seconds = 30.0;
  int max_players = 512;
  bool open_registration = true;
  std::uint32_t argon2_kib = 4096;
  ChatConfig chat;
  std::vector<ZoneDef> zones;

  const ZoneDef* zone(const std::string& id) const;
};

// Read <project>/mmo.json (or derive one zone from scene.json).
bool load_world_config(const std::string& project_dir, WorldConfig* out, std::string* error);

// Write a sample mmo.json (two zones, portals, pickups, blockers) into a
// project folder: the Town Hall panel's "Sample world" button, the smokes.
bool write_sample_world(const std::string& project_dir, std::string* error);

// One movement step: intent (mx, my) clamped to length 1, speed px/s for dt
// seconds, X then Y against the blockers, clamped to the bounds. Both ends
// run exactly this.
void step_rider(const ZoneGeometry& geo, float speed, float dt, float mx, float my, float* x, float* y);
// Portal under (x, y) or nullptr.
const Portal* portal_at(const ZoneGeometry& geo, float x, float y);

// Uniform grid over one zone: cell = interest radius. Rebuilt per tick.
class InterestGrid {
 public:
  void reset(float cell);
  void insert(std::uint64_t id, float x, float y);
  // Ids within `radius` of (x, y) (exact distance check).
  void query(float x, float y, float radius, std::vector<std::uint64_t>* out) const;
  std::size_t size() const { return count_; }

 private:
  struct Item {
    std::uint64_t id;
    float x, y;
  };
  static std::int64_t key(int cx, int cy) { return (static_cast<std::int64_t>(cx) << 32) ^ static_cast<std::uint32_t>(cy); }
  float cell_ = 640.0f;
  std::unordered_map<std::int64_t, std::vector<Item>> cells_;
  std::size_t count_ = 0;
};

}  // namespace mmo
}  // namespace tombstone
}  // namespace ts
