#pragma once

// Airship parts (ts.shipyard): what a ship is built from. Defaults are
// compiled in and written to <project>/shipyard/parts.json; a project's
// copy is read back, so new parts or retuned numbers are data edits.
//
// A part covers w x h grid cells (16 px each) at rotation 0 and turns in
// quarter steps (rot 0..3, clockwise). Engines push the ship toward their
// "front" (rotation 0 = up the grid, -y) from the nozzle at their back.

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace shipyard {

inline constexpr float kCell = 16.0f;  // world px per grid cell

enum class PartKind : std::uint8_t {
  Hull,
  Armor,
  Engine,
  Reactor,
  Quarters,
  Gun,
  Shield,
  Storage,
  Corridor,
};
inline constexpr int kPartKinds = 9;
const char* part_kind_id(PartKind k);    // "hull", "engine", ...
const char* part_kind_name(PartKind k);  // "Hull plating", ...
bool part_kind_from_id(const std::string& id, PartKind* out);

struct PartDef {
  std::string id;    // "steam_engine"
  std::string name;  // "Steam paddle engine"
  PartKind kind = PartKind::Hull;
  int w = 1, h = 1;     // footprint in cells at rotation 0
  float mass = 1.0f;    // tons
  int hp = 40;
  float power = 0.0f;   // + produces, - uses (steam units)
  int crew = 0;         // + berths (quarters), - hands needed to run it
  // Engines.
  float thrust = 0.0f;  // force (ton px / s^2) toward the part's front
  // Guns.
  float range = 0.0f;      // px
  int damage = 0;          // per shot
  float fire_rate = 0.0f;  // shots / s
  float shot_speed = 520.0f;
  // Shields.
  float shield = 0.0f;        // bubble strength (damage it soaks)
  float shield_regen = 0.0f;  // per s while powered
  // Storage.
  int capacity = 0;
  float color[3] = {0.6f, 0.5f, 0.4f};
};

struct PartLibrary {
  std::vector<PartDef> parts;
  // -1 when unknown.
  int find(const std::string& id) const;
  const PartDef* get(int index) const {
    return index >= 0 && index < static_cast<int>(parts.size())
               ? &parts[static_cast<std::size_t>(index)]
               : nullptr;
  }

  // The compiled-in defaults (also written to parts.json).
  static PartLibrary defaults();
  // Parse parts.json text. Bad entries are skipped with a warning; false
  // when nothing usable is left.
  bool parse(const std::string& text, std::string* error, std::vector<std::string>* warnings);
  // <project>/shipyard/parts.json, falling back to the defaults when the
  // file is missing (warning when it is broken).
  bool load(const std::string& project_dir, std::string* error,
            std::vector<std::string>* warnings);
};

// The default parts.json text.
const char* default_parts_json();
// <project>/shipyard (created on demand).
std::string shipyard_dir(const std::string& project_dir);

}  // namespace shipyard
}  // namespace tombstone
}  // namespace ts
