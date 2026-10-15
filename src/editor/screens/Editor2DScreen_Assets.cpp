// Asset Browser wiring for the 2D editor: the shared panel and database
// (editor/assets/AssetBrowser.h) plus what a rename / hot reload means for
// the open scene.
#include "editor/screens/Editor2DScreen.h"

#include "editor/assets/AssetLibrary.h"
#include "editor/workspace/SceneIO.h"

#include <iostream>

namespace ts {
namespace tombstone {
namespace editor {

bool Editor2DScreen::reload_scene_from_disk() {
  if (project_.path.empty()) return false;
  std::string err;
  if (!scene_io::load(workspace_, scene_path_, &err)) {
    note("Reload after asset move failed: " + err);
    return false;
  }
  // Paths inside the old undo snapshots no longer exist.
  workspace_.clear_history();
  dirty_ = false;
  return true;
}

void Editor2DScreen::open_asset_browser() {
  AssetBrowserHost host;
  host.before_disk_change = [this] {
    std::string err;
    if (!is_playing()) save_scene(&err);
  };
  host.after_refs_changed = [this] { reload_scene_from_disk(); };
  host.on_assets_changed = [this](const std::vector<std::string>& rels) {
    for (const std::string& rel : rels) {
      textures_.invalidate(asset_path(rel));
      textures_.invalidate(rel);
    }
  };
  host.payload_type = [](const assetdb::AssetDb::Entry& e) -> std::string {
    return e.meta.kind == assetdb::Kind::Texture ? assets::kDragPayload : kAssetAnyPayload;
  };
  host.note = [this](const std::string& m) { note(m); };
  asset_browser_.open(project_.path, std::move(host));
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
