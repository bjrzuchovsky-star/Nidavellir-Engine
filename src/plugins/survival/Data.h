#pragma once

// Items, recipes and creatures for Frontier Survival, read from the
// project's data files (JSON, editable by hand):
//
//   survival/items.json      {"items": [{"id", "name", "kind", ...}]}
//   survival/recipes.json    {"recipes": [{"id", "makes", "count",
//                              "needs": {"wood": 4}, "station": "workbench"}]}
//   survival/creatures.json  {"creatures": [{"id", "name", "hp", "biomes",
//                              "night_only", ...}]}
//   survival/world.json      {"seed", "world", "day_minutes"}
//
// A missing file falls back to the built-in defaults (the same text
// write_defaults() puts in a project).

#include "plugins/survival/Tiles.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace survival {

enum class ItemKind : std::uint8_t { Material, Tool, Weapon, Ranged, Ammo, Food, Placeable };
const char* item_kind_name(ItemKind k);

struct ItemDef {
  std::string id;           // "wood"
  std::string name;         // "Cottonwood logs"
  ItemKind kind = ItemKind::Material;
  float color[3] = {0.7f, 0.6f, 0.5f};
  int stack = 99;
  int damage = 0;           // melee (Weapon/Tool) or per shot (Ranged)
  int mine_power = 0;       // Tool: mining damage per swing
  int tier = 0;             // Tool: pick tier (1 stone, 2 iron)
  float cooldown = 0.4f;    // seconds between swings / shots
  float reach = 1.4f;       // tiles (melee)
  std::string ammo;         // Ranged: item id it fires
  int heal = 0;             // Food
  std::uint8_t places_object = kObjNone;  // Placeable
  std::uint8_t places_ground = kGroundNone;
};

struct RecipeDef {
  std::string id;
  int makes = -1;  // item index
  int count = 1;
  std::vector<std::pair<int, int>> needs;  // item index, count
  std::uint8_t station = kObjNone;          // object needed nearby (none = by hand)
};

struct CreatureDef {
  std::string id;           // "coyote"
  std::string name;         // "Coyote"
  int hp = 10;
  float speed = 70.0f;      // px / s
  int damage = 6;
  float aggro = 7.0f;       // tiles
  float size = 0.8f;        // tiles
  float color[3] = {0.6f, 0.5f, 0.4f};
  bool biomes[kBiomeCount] = {true, true, true, true};
  bool night_only = false;
  bool friendly = false;    // settlers: never attack, never attacked
  int weight = 10;          // spawn weight among eligible creatures
  int drop = -1;            // item index
  int drop_count = 1;
};

struct WorldSettings {
  std::uint64_t seed = 1849;
  std::string world = "frontier";  // saves/<world>/
  float day_minutes = 8.0f;         // a full day + night
};

struct GameData {
  std::vector<ItemDef> items;
  std::vector<RecipeDef> recipes;
  std::vector<CreatureDef> creatures;
  WorldSettings world;
  std::uint64_t hash = 0;  // of the data text (server / client check)

  int item(const std::string& id) const;      // -1 when unknown
  int creature(const std::string& id) const;
  int recipe(const std::string& id) const;
  const ItemDef* item_def(int index) const {
    return index >= 0 && index < static_cast<int>(items.size()) ? &items[static_cast<std::size_t>(index)]
                                                                : nullptr;
  }

  // Parse the four documents (empty text = built-in default). Problems
  // (unknown ids...) are appended to *warnings; a document that does not
  // parse fails with *error.
  bool parse(const std::string& items_json, const std::string& recipes_json,
             const std::string& creatures_json, const std::string& world_json,
             std::string* error, std::vector<std::string>* warnings);
  // <project>/survival/*.json, defaults for missing files.
  bool load(const std::string& project_dir, std::string* error,
            std::vector<std::string>* warnings);
  static GameData defaults();
};

const char* default_items_json();
const char* default_recipes_json();
const char* default_creatures_json();
std::string default_world_json(std::uint64_t seed = 1849);

// Write the default data files into <project>/survival/ (existing files
// are kept unless overwrite).
bool write_default_data(const std::string& project_dir, bool overwrite, std::string* error);
// Rewrite world.json with a new seed (editor Prospector).
bool write_world_settings(const std::string& project_dir, const WorldSettings& w,
                          std::string* error);

}  // namespace survival
}  // namespace tombstone
}  // namespace ts
