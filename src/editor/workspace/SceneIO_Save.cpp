#include "editor/workspace/SceneIO.h"

#include <string>

namespace ts {
namespace tombstone {
namespace editor {
namespace scene_io {

scene_json::SceneDoc to_doc(const Workspace2D& workspace) {
  scene_json::SceneDoc doc;
  doc.version = kSceneVersion;
  doc.entities = workspace.entities();
  doc.tile_solidity = workspace.tile_solidity();
  doc.lighting = workspace.lighting();
  doc.pan_x = workspace.pan_x();
  doc.pan_y = workspace.pan_y();
  doc.zoom = workspace.zoom();
  doc.show_grid = workspace.show_grid();
  doc.grid_size = workspace.grid_size();
  doc.snap = workspace.snap_enabled();
  doc.selected_id = workspace.selected_id();
  doc.selection = workspace.selection();
  return doc;
}

bool save(const Workspace2D& workspace, const std::string& scene_path,
          std::string* error_out) {
  return scene_json::save_file(scene_path, to_doc(workspace), error_out);
}

}  // namespace scene_io
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
