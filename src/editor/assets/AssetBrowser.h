#pragma once

// Asset Browser panel shared by the 2D and 3D editors (Dusk theme): folder
// tree, thumbnail grid with search and type filter, drag into the scene
// (the host picks the payload), drag onto a folder to move, rename /
// delete with "in use" warnings, find references, per-asset import
// settings (.meta) and hot reload of changed files.

#include "editor/assets/AssetDb.h"
#include "gfx/TextureCache.h"

#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

// ImGui payload the browser always accepts on folders (data = rel path).
inline constexpr const char* kAssetAnyPayload = "TS_ASSET_ANY";

struct AssetBrowserHost {
  // Called before files are renamed / references rewritten (save the open
  // scene so the rewrite sees it) and after (reload it).
  std::function<void()> before_disk_change;
  std::function<void()> after_refs_changed;
  // Content changed on disk (hot reload): rel paths.
  std::function<void(const std::vector<std::string>&)> on_assets_changed;
  // Drag payload type for an asset (kAssetAnyPayload when nothing better).
  std::function<std::string(const assetdb::AssetDb::Entry&)> payload_type;
  std::function<void(const std::string&)> note;
};

class AssetBrowser {
 public:
  void open(const std::string& project_dir, AssetBrowserHost host);
  assetdb::AssetDb& db() { return db_; }
  const assetdb::AssetDb& db() const { return db_; }

  // Rescan now (hot reload, moves outside the editor). Returns the report;
  // references are fixed and the host notified.
  assetdb::AssetDb::ScanReport rescan();
  // Throttled rescan from the UI loop.
  void poll(double now, double interval = 1.5);

  // Editor operations with host callbacks (also what the UI calls).
  bool move(const std::string& from, const std::string& to, std::string* error = nullptr);
  bool remove(const std::string& rel, bool force, std::vector<assetdb::Ref>* refs = nullptr,
              std::string* error = nullptr);

  void draw();
  const std::string& selected() const { return selected_; }
  void select(const std::string& rel) { selected_ = rel; }

 private:
  void draw_folders();
  void draw_grid();
  void draw_details();
  void draw_settings(const assetdb::AssetDb::Entry& e);
  bool tile(const assetdb::AssetDb::Entry& e, float size);
  void accept_move_into(const std::string& folder);

  assetdb::AssetDb db_;
  AssetBrowserHost host_;
  gfx::TextureCache thumbs_;
  std::string folder_ = "";  // "" = everything
  std::string selected_;
  char search_[96] = "";
  int filter_ = 0;  // 0 all, 1 textures, 2 models, 3 audio, 4 fonts
  float tile_size_ = 72.0f;
  double last_poll_ = -100.0;
  char rename_[256] = "";
  char new_folder_[128] = "";
  std::vector<assetdb::Ref> refs_shown_;
  std::string refs_for_;
  std::string delete_pending_;
  std::vector<assetdb::Ref> delete_refs_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
