#include "editor/workspace/SceneIO.h"

#include "scene/CameraMigrate.h"

#include <string>
#include <utility>

namespace ts {
namespace tombstone {
namespace editor {
namespace scene_io {

// Parsing (and the v1 -> v2 -> v3 -> v4 upgrades) lives in scene/SceneJson.cpp;
// this side only moves the document into the editor workspace.

void apply_doc(Workspace2D& workspace, scene_json::SceneDoc doc) {
  workspace.replace_scene(std::move(doc.entities), doc.selected_id, doc.pan_x,
                          doc.pan_y, doc.zoom, doc.show_grid);
  workspace.set_tile_solidity(std::move(doc.tile_solidity));
  workspace.set_lighting(doc.lighting);
  // Optional editor-tool state (older scene.json files simply omit these).
  if (doc.grid_size) {
    workspace.set_grid_size(*doc.grid_size);
  }
  if (doc.snap) {
    workspace.set_snap_enabled(*doc.snap);
  }
  if (!doc.selection.empty()) {
    workspace.set_selection(doc.selection, workspace.selected_id());
  }
}

bool load(Workspace2D& workspace, const std::string& scene_path,
          std::string* error_out, std::vector<std::string>* camera_log) {
  scene_json::SceneDoc doc;
  if (!scene_json::load_file(scene_path, &doc, error_out)) {
    return false;
  }
  if (camera_log) camera_migrate::migrate_2d(doc.version, &doc.entities, camera_log);
  apply_doc(workspace, std::move(doc));
  return true;
}

}  // namespace scene_io
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
