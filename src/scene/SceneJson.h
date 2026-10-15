#pragma once

// scene.json reader / writer shared by the editor (scene_io) and the
// runtime (ts_game, Play mode). Text in, text out; no UI.

#include "scene/JsonTree.h"
#include "scene/SceneData.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace scene_json {

// Format version written by write(). 1 = entities only; 2 = optional
// "tilemap" / "sprite" objects; 3 = optional "player" / "camera" / "spawn";
// 4 = optional "collider" plus the scene-level "tile_solidity" table;
// 5 = optional "animator"; 6 = optional "script"; 7 = optional editor
// "rotation" / "parent" / "locked" / "hidden"; 8 = "audio"; 9 =
// "particles" / "light" / "lighting"; 10 = optional Box2D "body" / "joint"
// and collider "shape"; 11 = optional "prefab" link. Older files still load
// (see upgrade notes in SceneJson.cpp).
inline constexpr int kSceneVersion = 11;

// Whole file: entities plus the editor's view state. The runtime only
// reads `entities`; the rest round-trips untouched.
struct SceneDoc {
  int version = kSceneVersion;  // as read (1 when the key is absent)
  std::vector<Entity2D> entities;
  // Which tile ids block movement, per tileset (v4; defaults when absent).
  TileSolidity tile_solidity;
  // Ambient / day-night / lightmap settings (v9 "lighting").
  fx::Lighting2D lighting;
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  float zoom = 1.0f;
  bool show_grid = true;
  std::optional<float> grid_size;
  std::optional<bool> snap;
  std::optional<std::uint64_t> selected_id;
  std::vector<std::uint64_t> selection;
};

// <project_dir>/scene.json
std::string scene_path_for_project(const std::string& project_dir);

// Parse scene.json text. Applies the v1 -> v2 TileMap, v2 -> v3 actor and
// v3 -> v4 player collider upgrades. `source` only labels error messages.
bool parse(const std::string& text, SceneDoc* doc, std::string* error_out,
           const std::string& source = "scene.json");
// Parents that are missing or loop go back to the root (parse() does it).
void repair_parents(std::vector<Entity2D>& entities);
// Serialise (always the current version). Deterministic: same doc, same
// bytes.
std::string write(const SceneDoc& doc);

// One entity as a JSON object (the scene.json entity text, parsed) and
// back (false + error on a malformed entity). Prefab tools use these.
json::Value entity_to_json(const Entity2D& e);
bool entity_from_json(const json::Value& v, Entity2D* out,
                      std::string* error_out = nullptr);
std::vector<json::Value> entities_to_json(const std::vector<Entity2D>& entities);
bool entities_from_json(const std::vector<json::Value>& values,
                        std::vector<Entity2D>* out,
                        std::string* error_out = nullptr);

bool load_file(const std::string& path, SceneDoc* doc,
               std::string* error_out = nullptr);
// Creates parent folders as needed.
bool save_file(const std::string& path, const SceneDoc& doc,
               std::string* error_out = nullptr);

}  // namespace scene_json
}  // namespace tombstone
}  // namespace ts
