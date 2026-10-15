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
  };
  host.payload_type = [](const assetdb::AssetDb::Entry& e) -> std::string {
    return e.meta.kind == assetdb::Kind::Font ? kAssetAnyPayload : "TS_ASSET3D";
  };
  host.note = [this](const std::string& m) { note(m); };
  asset_browser_.open(project_.path, std::move(host));
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
