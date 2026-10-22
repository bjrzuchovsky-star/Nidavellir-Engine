#pragma once

#include "editor/workspace/Workspace2D.h"
#include "scene/SceneJson.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace scene_io {

// scene.json format version written by save(); the format itself lives in
// scene/SceneJson (shared with the runtime). 1 = entities only; 2 = adds
// "tilemap" / "sprite"; 3 = adds "player" / "camera" / "spawn"; 4 = adds
// "collider" and the scene-level "tile_solidity" table; 5 = adds "animator".
inline constexpr int kSceneVersion = scene_json::kSceneVersion;

// Per-project Editor2D persistence: <project_dir>/scene.json
inline std::string scene_path_for_project(const std::string& project_dir) {
  return scene_json::scene_path_for_project(project_dir);
}

// Workspace <-> document (entities + view state). to_doc() is also what
// --smoke uses to compare workspaces byte for byte.
scene_json::SceneDoc to_doc(const Workspace2D& workspace);
void apply_doc(Workspace2D& workspace, scene_json::SceneDoc doc);

// Load entities + view state into workspace. Returns false if missing/invalid.
// `camera_log` (optional): run the v13 camera migration (docs/camera.md)
// on a v12- scene; one line per change goes there (empty = none).
bool load(Workspace2D& workspace, const std::string& scene_path,
          std::string* error_out = nullptr, std::vector<std::string>* camera_log = nullptr);

// Write workspace entities + view state. Creates parent dirs as needed.
bool save(const Workspace2D& workspace, const std::string& scene_path,
          std::string* error_out = nullptr);

}  // namespace scene_io
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
