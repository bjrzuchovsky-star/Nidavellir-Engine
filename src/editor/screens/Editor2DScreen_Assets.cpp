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
  host.send_to_vault = [this](const std::string& rel) {
    show_vault_ = true;
    vault_panel_.send_to_vault(rel);
  };
  host.vault_badge = [this](const std::string& rel) { return vault_panel_.badge(rel); };
  host.vault_origin = [this](const std::string& rel) { return vault_panel_.origin_label(rel); };
  host.on_vault_drop = [this](const std::string& item) { vault_panel_.pull_then(item, {}); };
  host.scripts = [this] { return scripts_; };
  host.open_script = [this](const std::string& rel, int which) {
    std::string msg;
    const ExternalEditor ed = which == 1 ? ExternalEditor::VSCode
                              : which == 2 ? ExternalEditor::VisualStudio
                                           : ExternalEditor::Auto;
    open_script_external(ed, project_.path, project_.name, rel, &msg);
    note(msg);
  };
  asset_browser_.open(project_.path, std::move(host));

  VaultHost vh;
  vh.db = [this] { return &asset_browser_.db(); };
  vh.selected_asset = [this] { return asset_browser_.selected(); };
  vh.prefabs = [this] { return prefabs_.list(); };
  vh.after_pull = [this] {
    asset_browser_.rescan();
    refresh_assets();
  };
  vh.note = [this](const std::string& m) { note(m); };
  vh.dimension = "2d";
  vault_panel_.open(project_.path, std::move(vh));
}

void Editor2DScreen::drop_vault_item(const std::string& item, float wx, float wy) {
  vault_panel_.pull_then(item, [this, wx, wy](const vaultp::PullResult& r) {
    if (r.kind == "texture") {
      create_sprite_at(r.main, wx, wy);
    } else if (r.kind == "prefab") {
      place_prefab(r.main, wx, wy);
    } else {
      note("Pulled " + r.main + " (only images and prefabs place in a 2D scene)");
    }
  });
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
