#pragma once

// Asset Browser panel shared by the 2D and 3D editors (Dusk theme): folder
// tree, thumbnail grid with search and type filter, drag into the scene
// (the host picks the payload), drag onto a folder to move, rename /
// delete with "in use" warnings, find references, per-asset import
// settings (.meta, kept in the claim's .tombstone/meta folder), hot reload
// of changed files, imports from outside (Import... button, files dropped
// on the window) sorted into type folders (ImportSort.h), orphaned .meta
// clean-up.

#include "editor/assets/AssetDb.h"
#include "editor/assets/GmaImport.h"
#include "editor/assets/ImportSort.h"
#include "editor/ui/FolderBrowser.h"
#include "gfx/TextureCache.h"

#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

// ImGui payload the browser always accepts on folders (data = rel path).
inline constexpr const char* kAssetAnyPayload = "TS_ASSET_ANY";
// A Vault tile (data = NUL-terminated item id): dropped on the browser, it is pulled.
inline constexpr const char* kVaultItemPayload = "TS_VAULT_ITEM";

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
  // The Vault (editor/vault/VaultPanel.h): right-click > Send to Vault,
  // origin / "update available" badges, and Vault tiles dropped here.
  std::function<void(const std::string& rel)> send_to_vault;
  std::function<int(const std::string& rel)> vault_badge;  // 0 none, 1 pulled, 2 update, 3 sent
  std::function<std::string(const std::string& rel)> vault_origin;
  std::function<void(const std::string& item)> on_vault_drop;
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
  // Copy outside files / folders in (dest_folder "" = sorted by type, see
  // ImportSort.h), then rescan so they get their .meta. Returns the result.
  importsort::Result import_files(const std::vector<std::string>& sources, const std::string& dest_folder = {});
  const importsort::Settings& import_settings() const { return import_settings_; }
  void set_import_settings(const importsort::Settings& s);
  // Orphaned .meta files from the last scan (asset paths).
  const std::vector<std::string>& orphans() const { return orphans_; }

  void draw();
  // Source addon import (.gma): opens the picker on a file (drops).
  void import_gma(const std::string& path) { gma_dialog_.open_file(db_.project_dir(), path); }
  // Reimport and tell the host (the scene reloads the model / texture).
  void reimport(const std::string& rel);
  const std::string& selected() const { return selected_; }
  void select(const std::string& rel) { selected_ = rel; }

 private:
  void draw_folders();
  void draw_grid();
  void draw_details();
  void draw_settings(const assetdb::AssetDb::Entry& e);
  bool tile(const assetdb::AssetDb::Entry& e, float size);
  void accept_move_into(const std::string& folder);
  void accept_vault_drop();
  void take_os_drop(const std::string& folder);
  void draw_import_popup();

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
  // Model import text fields (kept while typing; owner = "rel|key").
  char material_path_[1024] = "";
  std::string material_path_for_;
  char blender_path_[512] = "";
  std::string blender_path_for_;
  importsort::Settings import_settings_;
  std::vector<std::string> orphans_;
  bool orphans_popup_ = false;
  FolderBrowser import_picker_;
  bool import_pick_folder_ = false;
  std::string drop_folder_;  // folder row under the mouse this frame ("" none)
  bool grid_hovered_ = false;
  double drop_seen_ = -1.0;
  std::string delete_pending_;
  std::vector<assetdb::Ref> delete_refs_;
  gmaimport::Dialog gma_dialog_;
  FolderBrowser relink_picker_;
  std::string relink_for_;  // "rel|material" while the relink picker is open
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
