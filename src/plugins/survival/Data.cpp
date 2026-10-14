#include "plugins/survival/Data.h"

#include "scene/JsonTree.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ts {
namespace tombstone {
namespace survival {

namespace fs = std::filesystem;

namespace {

const char* const kItems = R"({
  "items": [
    {"id": "wood", "name": "Cottonwood logs", "kind": "material", "color": [0.55, 0.38, 0.22]},
    {"id": "stone", "name": "Fieldstone", "kind": "material", "color": [0.58, 0.56, 0.52]},
    {"id": "fiber", "name": "Sage fiber", "kind": "material", "color": [0.62, 0.66, 0.48]},
    {"id": "hide", "name": "Rawhide", "kind": "material", "color": [0.66, 0.50, 0.36]},
    {"id": "copper_ore", "name": "Copper ore", "kind": "material", "color": [0.80, 0.47, 0.25]},
    {"id": "iron_ore", "name": "Iron ore", "kind": "material", "color": [0.62, 0.58, 0.56]},
    {"id": "gold_nugget", "name": "Gold nugget", "kind": "material", "color": [0.95, 0.80, 0.25]},
    {"id": "copper_bar", "name": "Copper bar", "kind": "material", "color": [0.90, 0.55, 0.30]},
    {"id": "iron_bar", "name": "Iron bar", "kind": "material", "color": [0.78, 0.78, 0.80]},
    {"id": "cactus_flesh", "name": "Cactus pulp", "kind": "food", "heal": 8, "color": [0.40, 0.70, 0.40]},
    {"id": "jerky", "name": "Trail jerky", "kind": "food", "heal": 30, "color": [0.55, 0.25, 0.18]},
    {"id": "stone_pick", "name": "Stone pick", "kind": "tool", "mine_power": 3, "tier": 1, "damage": 4, "stack": 1, "color": [0.60, 0.58, 0.55]},
    {"id": "copper_pick", "name": "Copper pick", "kind": "tool", "mine_power": 4, "tier": 2, "damage": 5, "stack": 1, "color": [0.90, 0.55, 0.30]},
    {"id": "iron_pick", "name": "Iron pick", "kind": "tool", "mine_power": 6, "tier": 2, "damage": 6, "stack": 1, "color": [0.80, 0.80, 0.85]},
    {"id": "knife", "name": "Bowie knife", "kind": "weapon", "damage": 7, "cooldown": 0.3, "stack": 1, "color": [0.75, 0.75, 0.70]},
    {"id": "saber", "name": "Cavalry saber", "kind": "weapon", "damage": 14, "cooldown": 0.45, "reach": 1.8, "stack": 1, "color": [0.85, 0.85, 0.90]},
    {"id": "bow", "name": "Hunting bow", "kind": "ranged", "damage": 8, "ammo": "arrow", "cooldown": 0.6, "stack": 1, "color": [0.60, 0.42, 0.24]},
    {"id": "revolver", "name": "Six-shooter", "kind": "ranged", "damage": 16, "ammo": "bullet", "cooldown": 0.35, "stack": 1, "color": [0.35, 0.33, 0.32]},
    {"id": "arrow", "name": "Arrows", "kind": "ammo", "color": [0.70, 0.60, 0.45]},
    {"id": "bullet", "name": "Cartridges", "kind": "ammo", "color": [0.85, 0.65, 0.30]},
    {"id": "wood_wall", "name": "Plank wall", "kind": "placeable", "places": "wood_wall", "color": [0.48, 0.33, 0.19]},
    {"id": "door", "name": "Swinging door", "kind": "placeable", "places": "door", "color": [0.55, 0.38, 0.22]},
    {"id": "planks", "name": "Floor planks", "kind": "placeable", "places": "planks", "color": [0.62, 0.45, 0.27]},
    {"id": "workbench", "name": "Workbench", "kind": "placeable", "places": "workbench", "stack": 9, "color": [0.58, 0.42, 0.26]},
    {"id": "forge", "name": "Forge", "kind": "placeable", "places": "forge", "stack": 9, "color": [0.36, 0.33, 0.32]},
    {"id": "campfire", "name": "Campfire", "kind": "placeable", "places": "campfire", "stack": 9, "color": [0.90, 0.45, 0.18]},
    {"id": "torch", "name": "Torch", "kind": "placeable", "places": "torch", "color": [0.95, 0.70, 0.30]},
    {"id": "bed", "name": "Bedroll", "kind": "placeable", "places": "bed", "stack": 9, "color": [0.65, 0.25, 0.22]}
  ]
}
)";

const char* const kRecipes = R"({
  "recipes": [
    {"id": "workbench", "makes": "workbench", "needs": {"wood": 6}},
    {"id": "campfire", "makes": "campfire", "needs": {"wood": 3, "stone": 2}},
    {"id": "torch", "makes": "torch", "count": 4, "needs": {"wood": 1, "fiber": 1}},
    {"id": "stone_pick", "makes": "stone_pick", "needs": {"wood": 3, "stone": 3}, "station": "workbench"},
    {"id": "knife", "makes": "knife", "needs": {"wood": 1, "stone": 2}, "station": "workbench"},
    {"id": "bow", "makes": "bow", "needs": {"wood": 3, "fiber": 4}, "station": "workbench"},
    {"id": "arrow", "makes": "arrow", "count": 8, "needs": {"wood": 1, "stone": 1}, "station": "workbench"},
    {"id": "wood_wall", "makes": "wood_wall", "count": 4, "needs": {"wood": 2}, "station": "workbench"},
    {"id": "door", "makes": "door", "needs": {"wood": 4}, "station": "workbench"},
    {"id": "planks", "makes": "planks", "count": 4, "needs": {"wood": 2}, "station": "workbench"},
    {"id": "bed", "makes": "bed", "needs": {"wood": 4, "fiber": 6}, "station": "workbench"},
    {"id": "forge", "makes": "forge", "needs": {"stone": 12, "wood": 4}, "station": "workbench"},
    {"id": "copper_bar", "makes": "copper_bar", "needs": {"copper_ore": 2}, "station": "forge"},
    {"id": "iron_bar", "makes": "iron_bar", "needs": {"iron_ore": 2}, "station": "forge"},
    {"id": "copper_pick", "makes": "copper_pick", "needs": {"wood": 2, "copper_bar": 3}, "station": "forge"},
    {"id": "iron_pick", "makes": "iron_pick", "needs": {"wood": 2, "iron_bar": 3}, "station": "forge"},
    {"id": "saber", "makes": "saber", "needs": {"iron_bar": 4, "wood": 1}, "station": "forge"},
    {"id": "revolver", "makes": "revolver", "needs": {"iron_bar": 6, "gold_nugget": 2}, "station": "forge"},
    {"id": "bullet", "makes": "bullet", "count": 12, "needs": {"copper_bar": 1}, "station": "forge"},
    {"id": "jerky", "makes": "jerky", "needs": {"hide": 1}, "station": "campfire"}
  ]
}
)";

const char* const kCreatures = R"({
  "creatures": [
    {"id": "coyote", "name": "Coyote", "hp": 14, "damage": 5, "speed": 85, "aggro": 7,
     "biomes": ["prairie", "desert", "badlands"], "weight": 10, "drop": "hide", "color": [0.66, 0.56, 0.42]},
    {"id": "rattlesnake", "name": "Rattlesnake", "hp": 8, "damage": 7, "speed": 55, "aggro": 4,
     "biomes": ["desert"], "weight": 8, "size": 0.6, "drop": "hide", "color": [0.62, 0.55, 0.30]},
    {"id": "scorpion", "name": "Giant scorpion", "hp": 12, "damage": 8, "speed": 60, "aggro": 5,
     "biomes": ["badlands", "desert"], "weight": 6, "size": 0.7, "color": [0.20, 0.16, 0.14]},
    {"id": "cave_bat", "name": "Cave bat", "hp": 6, "damage": 4, "speed": 110, "aggro": 8,
     "biomes": ["caves"], "weight": 10, "size": 0.55, "color": [0.30, 0.24, 0.30]},
    {"id": "bandit", "name": "Night rider", "hp": 30, "damage": 10, "speed": 78, "aggro": 9,
     "biomes": ["prairie", "desert", "badlands"], "night_only": true, "weight": 8, "size": 0.9,
     "drop": "gold_nugget", "color": [0.25, 0.20, 0.22]},
    {"id": "settler", "name": "Settler", "hp": 40, "damage": 0, "speed": 40, "aggro": 0,
     "friendly": true, "weight": 0, "size": 0.85, "color": [0.40, 0.55, 0.75]}
  ]
}
)";

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::uint64_t fnv(std::uint64_t h, const std::string& s) {
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

bool parse_doc(const std::string& text, const char* fallback, const char* what, json::Value* out,
               std::string* error) {
  std::string err;
  if (!json::parse(text.empty() ? std::string(fallback) : text, out, &err)) {
    if (error) *error = std::string(what) + ": " + err;
    return false;
  }
  return true;
}

void read_color(const json::Value& v, float* rgb) { v.floats("color", rgb, 3); }

bool write_file(const fs::path& p, const std::string& text, bool overwrite, std::string* error) {
  std::error_code ec;
  if (!overwrite && fs::exists(p, ec)) return true;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
  if (!out) {
    if (error) *error = "could not write " + p.string();
    return false;
  }
  return true;
}

}  // namespace

const char* item_kind_name(ItemKind k) {
  switch (k) {
    case ItemKind::Tool: return "tool";
    case ItemKind::Weapon: return "weapon";
    case ItemKind::Ranged: return "ranged";
    case ItemKind::Ammo: return "ammo";
    case ItemKind::Food: return "food";
    case ItemKind::Placeable: return "placeable";
    case ItemKind::Material: break;
  }
  return "material";
}

const char* default_items_json() { return kItems; }
const char* default_recipes_json() { return kRecipes; }
const char* default_creatures_json() { return kCreatures; }

std::string default_world_json(std::uint64_t seed) {
  return "{\n  \"seed\": " + std::to_string(seed) +
         ",\n  \"world\": \"frontier\",\n  \"day_minutes\": 8\n}\n";
}

int GameData::item(const std::string& id) const {
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (items[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

int GameData::creature(const std::string& id) const {
  for (std::size_t i = 0; i < creatures.size(); ++i) {
    if (creatures[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

int GameData::recipe(const std::string& id) const {
  for (std::size_t i = 0; i < recipes.size(); ++i) {
    if (recipes[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

bool GameData::parse(const std::string& items_json, const std::string& recipes_json,
                     const std::string& creatures_json, const std::string& world_json,
                     std::string* error, std::vector<std::string>* warnings) {
  auto warn = [&](const std::string& w) {
    if (warnings) warnings->push_back(w);
  };
  json::Value it, rc, cr, wd;
  if (!parse_doc(items_json, kItems, "items.json", &it, error) ||
      !parse_doc(recipes_json, kRecipes, "recipes.json", &rc, error) ||
      !parse_doc(creatures_json, kCreatures, "creatures.json", &cr, error) ||
      !parse_doc(world_json, default_world_json().c_str(), "world.json", &wd, error)) {
    return false;
  }
  items.clear();
  recipes.clear();
  creatures.clear();
  for (const json::Value& v : it["items"].items()) {
    ItemDef d;
    d.id = v.str("id");
    if (d.id.empty() || item(d.id) >= 0) {
      warn("items.json: item without an id or a duplicate (" + d.id + ")");
      continue;
    }
    d.name = v.str("name", d.id);
    const std::string kind = v.str("kind", "material");
    for (int k = 0; k <= static_cast<int>(ItemKind::Placeable); ++k) {
      if (kind == item_kind_name(static_cast<ItemKind>(k))) d.kind = static_cast<ItemKind>(k);
    }
    read_color(v, d.color);
    d.stack = std::max(1, static_cast<int>(v.num("stack", 99)));
    d.damage = static_cast<int>(v.num("damage", 0));
    d.mine_power = static_cast<int>(v.num("mine_power", 0));
    d.tier = static_cast<int>(v.num("tier", 0));
    d.cooldown = std::max(0.05f, v.flt("cooldown", 0.4f));
    d.reach = std::max(0.5f, v.flt("reach", 1.4f));
    d.ammo = v.str("ammo");
    d.heal = static_cast<int>(v.num("heal", 0));
    const std::string places = v.str("places");
    if (!places.empty() && !object_by_id(places.c_str(), &d.places_object) &&
        !ground_by_id(places.c_str(), &d.places_ground)) {
      warn("items.json: " + d.id + " places unknown \"" + places + "\"");
    }
    items.push_back(std::move(d));
  }
  for (const json::Value& v : rc["recipes"].items()) {
    RecipeDef r;
    r.id = v.str("id", v.str("makes"));
    r.makes = item(v.str("makes"));
    r.count = std::max(1, static_cast<int>(v.num("count", 1)));
    if (r.makes < 0) {
      warn("recipes.json: " + r.id + " makes an unknown item");
      continue;
    }
    bool ok = true;
    for (const auto& [name, n] : v["needs"].members()) {
      const int idx = item(name);
      if (idx < 0) {
        warn("recipes.json: " + r.id + " needs unknown \"" + name + "\"");
        ok = false;
      }
      r.needs.push_back({idx, std::max(1, static_cast<int>(n.as_number(1)))});
    }
    const std::string station = v.str("station");
    if (!station.empty() && !object_by_id(station.c_str(), &r.station)) {
      warn("recipes.json: " + r.id + " wants unknown station \"" + station + "\"");
      ok = false;
    }
    if (ok) recipes.push_back(std::move(r));
  }
  for (const json::Value& v : cr["creatures"].items()) {
    CreatureDef c;
    c.id = v.str("id");
    if (c.id.empty()) continue;
    c.name = v.str("name", c.id);
    c.hp = std::max(1, static_cast<int>(v.num("hp", 10)));
    c.damage = static_cast<int>(v.num("damage", 5));
    c.speed = v.flt("speed", 70.0f);
    c.aggro = v.flt("aggro", 7.0f);
    c.size = std::clamp(v.flt("size", 0.8f), 0.3f, 2.0f);
    read_color(v, c.color);
    c.night_only = v.flag("night_only", false);
    c.friendly = v.flag("friendly", false);
    c.weight = std::max(0, static_cast<int>(v.num("weight", 10)));
    if (v["biomes"].is_array()) {
      for (bool& b : c.biomes) b = false;
      for (const json::Value& b : v["biomes"].items()) {
        Biome bio;
        if (biome_from_name(b.as_string_or("").c_str(), &bio)) c.biomes[static_cast<int>(bio)] = true;
      }
    }
    const std::string drop = v.str("drop");
    c.drop = drop.empty() ? -1 : item(drop);
    if (!drop.empty() && c.drop < 0) warn("creatures.json: " + c.id + " drops unknown " + drop);
    c.drop_count = std::max(1, static_cast<int>(v.num("drop_count", 1)));
    creatures.push_back(std::move(c));
  }
  world.seed = static_cast<std::uint64_t>(wd.num("seed", 1849));
  world.world = wd.str("world", "frontier");
  if (world.world.empty() || world.world.find_first_of("/\\.:") != std::string::npos) {
    world.world = "frontier";
  }
  world.day_minutes = std::clamp(wd.flt("day_minutes", 8.0f), 0.5f, 120.0f);
  std::uint64_t h = 1469598103934665603ull;
  h = fnv(h, items_json.empty() ? kItems : items_json);
  h = fnv(h, recipes_json.empty() ? kRecipes : recipes_json);
  h = fnv(h, creatures_json.empty() ? kCreatures : creatures_json);
  hash = h;
  if (item("wood") < 0) warn("items.json: no \"wood\"; trees will drop nothing");
  return true;
}

bool GameData::load(const std::string& project_dir, std::string* error,
                    std::vector<std::string>* warnings) {
  const fs::path dir = fs::path(project_dir) / "survival";
  return parse(read_text(dir / "items.json"), read_text(dir / "recipes.json"),
               read_text(dir / "creatures.json"), read_text(dir / "world.json"), error, warnings);
}

GameData GameData::defaults() {
  GameData d;
  d.parse("", "", "", "", nullptr, nullptr);
  return d;
}

bool write_default_data(const std::string& project_dir, bool overwrite, std::string* error) {
  const fs::path dir = fs::path(project_dir) / "survival";
  return write_file(dir / "items.json", kItems, overwrite, error) &&
         write_file(dir / "recipes.json", kRecipes, overwrite, error) &&
         write_file(dir / "creatures.json", kCreatures, overwrite, error) &&
         write_file(dir / "world.json", default_world_json(), overwrite, error);
}

bool write_world_settings(const std::string& project_dir, const WorldSettings& w,
                          std::string* error) {
  char day[32];
  std::snprintf(day, sizeof(day), "%g", static_cast<double>(w.day_minutes));
  const std::string text = "{\n  \"seed\": " + std::to_string(w.seed) + ",\n  \"world\": \"" +
                           w.world + "\",\n  \"day_minutes\": " + day + "\n}\n";
  return write_file(fs::path(project_dir) / "survival" / "world.json", text, true, error);
}

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
