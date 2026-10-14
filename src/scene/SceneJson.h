#pragma once

// scene.json reader / writer shared by the editor (scene_io) and the
// runtime (ts_game, Play mode). Text in, text out; no UI.

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
// "rotation" / "parent" / "locked" / "hidden". Older files still load
// (see upgrade notes in SceneJson.cpp).
inline constexpr int kSceneVersion = 8;

// Whole file: entities plus the editor's view state. The runtime only
// reads `entities`; the rest round-trips untouched.
struct SceneDoc {
  int version = kSceneVersion;  // as read (1 when the key is absent)
  std::vector<Entity2D> entities;
  // Which tile ids block movement, per tileset (v4; defaults when absent).
  TileSolidity tile_solidity;
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

bool load_file(const std::string& path, SceneDoc* doc,
               std::string* error_out = nullptr);
// Creates parent folders as needed.
bool save_file(const std::string& path, const SceneDoc& doc,
               std::string* error_out = nullptr);

}  // namespace scene_json
}  // namespace tombstone
}  // namespace ts
