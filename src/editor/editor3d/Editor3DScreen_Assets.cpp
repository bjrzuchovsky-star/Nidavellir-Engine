// Asset Browser wiring for the 3D editor (see editor/assets/AssetBrowser.h).
#include "editor/editor3d/Editor3DScreen.h"

namespace ts {
namespace tombstone {
namespace editor {

void Editor3DScreen::open_asset_browser() {
  AssetBrowserHost host;
  host.before_disk_change = [this] {
    std::string err;
    if (!playing()) save_scene(&err);
  };
  host.after_refs_changed = [this] {
    std::string err;
    if (!reload_scene(&err)) note("Reload after asset move failed: " + err, runtime::LogLevel::Error);
    rescan_assets();
  };
  host.on_assets_changed = [this](const std::vector<std::string>&) {
    if (view_) view_->clear();  // models / textures reload on next draw
    rescan_assets();
    refit_model_colliders(false);  // fitted colliders follow a reimport
  };
  host.payload_type = [](const assetdb::AssetDb::Entry& e) -> std::string {
    return e.meta.kind == assetdb::Kind::Font ? kAssetAnyPayload : "TS_ASSET3D";
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
  host.split_model = [this](const std::string& rel, bool recentre, bool fit) {
    return split_model_asset(rel, recentre, fit);
  };
  host.place_parts = [this](const std::string& rel, bool recentre, bool fit) {
    return place_model_parts(rel, {}, recentre, fit) != 0;
  };
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
    rescan_assets();
  };
  vh.note = [this](const std::string& m) { note(m); };
  vh.dimension = "3d";
  vault_panel_.open(project_.path, std::move(vh));
}

void Editor3DScreen::drop_vault_item(const std::string& item, const render3d::Vec3& pos) {
  vault_panel_.pull_then(item, [this, pos](const vaultp::PullResult& r) {
    if (r.kind == "prefab") drop_asset("prefab:" + r.main, pos);
    else if (r.kind == "model" || r.kind == "audio") drop_asset(r.main, pos);
    else note("Pulled " + r.main + " into the claim");
  });
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
