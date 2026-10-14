#include "plugins/shipyard/Parts.h"

#include "scene/JsonTree.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace shipyard {

namespace {

struct KindInfo {
  PartKind kind;
  const char* id;
  const char* name;
};

constexpr KindInfo kKinds[kPartKinds] = {
    {PartKind::Hull, "hull", "Hull plating"},     {PartKind::Armor, "armor", "Iron armor"},
    {PartKind::Engine, "engine", "Engine"},       {PartKind::Reactor, "reactor", "Boiler"},
    {PartKind::Quarters, "quarters", "Bunkhouse"}, {PartKind::Gun, "gun", "Gun turret"},
    {PartKind::Shield, "shield", "Shield vane"},  {PartKind::Storage, "storage", "Cargo hold"},
    {PartKind::Corridor, "corridor", "Gangway"},
};

const char* const kDefaultParts = R"({
  "parts": [
    {"id": "hull", "name": "Plank hull", "kind": "hull", "w": 1, "h": 1, "mass": 1.0, "hp": 40, "color": [0.56, 0.40, 0.25]},
    {"id": "armor", "name": "Riveted iron plate", "kind": "armor", "w": 1, "h": 1, "mass": 2.5, "hp": 140, "color": [0.45, 0.45, 0.48]},
    {"id": "gangway", "name": "Gangway", "kind": "corridor", "w": 1, "h": 1, "mass": 0.6, "hp": 30, "color": [0.70, 0.55, 0.34]},
    {"id": "steam_engine", "name": "Steam paddle engine", "kind": "engine", "w": 1, "h": 2, "mass": 3.0, "hp": 60, "power": -2, "crew": -1, "thrust": 900, "color": [0.72, 0.46, 0.24]},
    {"id": "trim_jet", "name": "Trim jet", "kind": "engine", "w": 1, "h": 1, "mass": 1.0, "hp": 35, "power": -0.5, "thrust": 480, "color": [0.80, 0.60, 0.30]},
    {"id": "boiler", "name": "Coal boiler", "kind": "reactor", "w": 2, "h": 2, "mass": 6.0, "hp": 120, "power": 12, "crew": -1, "color": [0.40, 0.30, 0.28]},
    {"id": "bunkhouse", "name": "Bunkhouse", "kind": "quarters", "w": 2, "h": 1, "mass": 2.0, "hp": 50, "crew": 6, "color": [0.62, 0.48, 0.36]},
    {"id": "gatling", "name": "Gatling turret", "kind": "gun", "w": 1, "h": 1, "mass": 2.0, "hp": 60, "power": -1, "range": 330, "damage": 9, "fire_rate": 5, "shot_speed": 560, "color": [0.30, 0.30, 0.32]},
    {"id": "cannon", "name": "Long Tom cannon", "kind": "gun", "w": 2, "h": 2, "mass": 6.0, "hp": 130, "power": -3, "crew": -2, "range": 460, "damage": 45, "fire_rate": 0.8, "shot_speed": 480, "color": [0.24, 0.22, 0.24]},
    {"id": "shield_vane", "name": "Tesla shield vane", "kind": "shield", "w": 1, "h": 2, "mass": 2.0, "hp": 50, "power": -2, "crew": -1, "shield": 160, "shield_regen": 14, "color": [0.35, 0.55, 0.75]},
    {"id": "cargo", "name": "Cargo hold", "kind": "storage", "w": 2, "h": 2, "mass": 3.0, "hp": 80, "capacity": 40, "color": [0.55, 0.45, 0.30]}
  ]
}
)";

std::string read_text(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

const char* part_kind_id(PartKind k) { return kKinds[static_cast<int>(k)].id; }
const char* part_kind_name(PartKind k) { return kKinds[static_cast<int>(k)].name; }

bool part_kind_from_id(const std::string& id, PartKind* out) {
  for (const KindInfo& k : kKinds) {
    if (id == k.id) {
      *out = k.kind;
      return true;
    }
  }
  return false;
}

const char* default_parts_json() { return kDefaultParts; }

std::string shipyard_dir(const std::string& project_dir) {
  return (std::filesystem::path(project_dir) / "shipyard").string();
}

int PartLibrary::find(const std::string& id) const {
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (parts[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

PartLibrary PartLibrary::defaults() {
  PartLibrary lib;
  std::string err;
  lib.parse(kDefaultParts, &err, nullptr);
  return lib;
}

bool PartLibrary::parse(const std::string& text, std::string* error,
                        std::vector<std::string>* warnings) {
  json::Value root;
  std::string err;
  if (!json::parse(text, &root, &err)) {
    if (error) *error = "parts.json: " + err;
    return false;
  }
  std::vector<PartDef> out;
  for (const json::Value& v : root["parts"].items()) {
    PartDef d;
    d.id = v.str("id");
    d.name = v.str("name", d.id);
    const std::string kind = v.str("kind");
    if (d.id.empty() || !part_kind_from_id(kind, &d.kind)) {
      if (warnings) warnings->push_back("parts.json: skipped a part with no id or kind '" + kind + "'");
      continue;
    }
    bool dup = false;
    for (const PartDef& o : out) dup |= o.id == d.id;
    if (dup) {
      if (warnings) warnings->push_back("parts.json: duplicate part '" + d.id + "' skipped");
      continue;
    }
    d.w = std::max(1, std::min(4, static_cast<int>(v.num("w", 1))));
    d.h = std::max(1, std::min(4, static_cast<int>(v.num("h", 1))));
    d.mass = std::max(0.05f, v.flt("mass", d.mass));
    d.hp = std::max(1, static_cast<int>(v.num("hp", d.hp)));
    d.power = v.flt("power", 0.0f);
    d.crew = static_cast<int>(v.num("crew", 0));
    d.thrust = std::max(0.0f, v.flt("thrust", 0.0f));
    d.range = std::max(0.0f, v.flt("range", 0.0f));
    d.damage = std::max(0, static_cast<int>(v.num("damage", 0)));
    d.fire_rate = std::max(0.0f, v.flt("fire_rate", 0.0f));
    d.shot_speed = std::max(50.0f, v.flt("shot_speed", d.shot_speed));
    d.shield = std::max(0.0f, v.flt("shield", 0.0f));
    d.shield_regen = std::max(0.0f, v.flt("shield_regen", 0.0f));
    d.capacity = std::max(0, static_cast<int>(v.num("capacity", 0)));
    v.floats("color", d.color, 3);
    out.push_back(std::move(d));
  }
  if (out.empty()) {
    if (error) *error = "parts.json has no usable parts";
    return false;
  }
  parts = std::move(out);
  return true;
}

bool PartLibrary::load(const std::string& project_dir, std::string* error,
                       std::vector<std::string>* warnings) {
  const std::filesystem::path p = std::filesystem::path(shipyard_dir(project_dir)) / "parts.json";
  const std::string text = project_dir.empty() ? std::string() : read_text(p);
  if (text.empty()) return parse(kDefaultParts, error, warnings);
  std::string err;
  if (parse(text, &err, warnings)) return true;
  if (warnings) warnings->push_back(err + " (using the built-in parts)");
  return parse(kDefaultParts, error, warnings);
}

}  // namespace shipyard
}  // namespace tombstone
}  // namespace ts
