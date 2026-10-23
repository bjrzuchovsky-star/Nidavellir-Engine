#pragma once

// The Vault panel (ImGui, Dusk theme) shared by the 2D and 3D editors:
// browse / search / filter the library with thumbnails, upload a selection
// from the Asset Browser ("Send to Vault", with name, tags, 2D/3D/VR,
// author, licence, version note, thumbnail), pull into the open claim
// (Pull button, or drag a tile into the Asset Browser or the scene),
// see which pulled items have an update, pin or update them. Library
// settings (local folder / server URL + API key) live in the editor
// settings. Network and disk work runs on a worker thread; the editors
// hold their Asset Browser polling while busy() is true.

#include "editor/assets/AssetBrowser.h"
#include "editor/assets/AssetDb.h"
#include "editor/ux/FriendlyError.h"
#include "editor/vault/VaultProject.h"
#include "gfx/TextureCache.h"
#include "vault/VaultTypes.h"

#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

// Vault tiles drag as kVaultItemPayload (editor/assets/AssetBrowser.h).

struct VaultHost {
  std::function<assetdb::AssetDb*()> db;               // the Asset Browser's (main thread)
  std::function<std::string()> selected_asset;         // Asset Browser selection ("" = none)
  std::function<std::vector<std::string>()> prefabs;   // project prefabs
  std::function<void()> after_pull;                    // rescan the Asset Browser
  std::function<void(const std::string&)> note;
  std::string dimension = "2d";                        // default dims tag for uploads
  // Vault manager (Project Manager, no claim open): edit / delete items and
  // versions, Add files... (loose files and OS drops), Pull into claim...
  bool manager = false;
  struct Claim {
    std::string name, path, kind;  // kind: "2D" / "3D" / "VR"
  };
  std::function<std::vector<Claim>()> claims;
  std::string scratch_dir;  // Add files staging ("" = config dir/vault_add)
};

// The Vault panel's error card: what went wrong, plus a Vault settings fix.
ux::FriendlyError friendly_vault_error(const std::string& raw, bool remote);

class VaultPanel {
 public:
  VaultPanel() = default;
  ~VaultPanel();
  VaultPanel(const VaultPanel&) = delete;
  VaultPanel& operator=(const VaultPanel&) = delete;

  void open(const std::string& project_dir, VaultHost host);
  // Per frame (also while hidden): finishes background jobs.
  void update();
  void draw();
  bool busy() const { return job_.valid(); }

  // Asset Browser hooks.
  void send_to_vault(const std::string& rel);
  // 0 = not from a Vault, 1 = pulled, 2 = pulled + update available, 3 = sent from here.
  int badge(const std::string& rel) const;
  std::string origin_label(const std::string& rel) const;
  // Pull (worker) then call `done` on the main thread (scene / browser drops).
  void pull_then(const std::string& item, std::function<void(const vaultp::PullResult&)> done);
  void refresh();
  // Point the panel at a library without touching the saved settings
  // (smoke, screenshots). backend: "local" | "remote".
  void use_library(const std::string& backend, const std::string& local_dir, const std::string& url,
                   const std::string& api_key);
  // Select an item (details pane) by id.
  void select_item(const std::string& id) { select(id); }

  // Vault manager.
  bool manager() const { return host_.manager; }
  // Opens the send dialog for files from outside any claim (Add files...,
  // OS drops). Empty = open it to browse / paste paths.
  void add_files(const std::vector<std::string>& paths);
  // Pull (worker) into a claim folder through the usual pull path.
  void pull_into(const std::string& claim_dir, const std::string& item, int version = 0);
  // Opens the metadata editor for the selected item (also the screenshot).
  void begin_edit();
  bool editing() const { return editing_; }
  const std::vector<vault::Item>& items() const { return items_; }
  const std::string& last_error() const { return error_; }
  const std::string& last_status() const { return status_line_; }

 private:
  struct JobResult {
    bool ok = true;
    std::string error;
    std::string message;
    bool listed = false;
    std::vector<vault::Item> items;
    std::vector<vaultp::ItemStatus> status;
    std::map<std::string, std::string> thumbs;  // sha -> cached png
    bool has_detail = false;
    vault::Item detail;
    bool pulled = false;
    vaultp::PullResult pull;
    bool changed_project = false;
  };
  using Job = std::function<JobResult()>;
  void start(const std::string& label, Job job, std::function<void(const JobResult&)> then = {});
  std::unique_ptr<vault::Backend> make_backend() const;
  void load_settings();
  void save_settings();
  void select(const std::string& id);
  void draw_toolbar();
  void draw_grid();
  void draw_details();
  void draw_settings_popup();
  void draw_upload_modal();
  bool tile(const vault::Item& it, float size);
  const vaultp::ItemStatus* status_of(const std::string& id) const;
  void replan_upload();
  void draw_manager_actions(const vault::Item& it);
  void draw_edit_form(const vault::Item& it);
  void draw_delete_modal();
  void draw_add_files_body();
  void start_add_files(const vaultp::PushOptions& opt);

  std::string project_dir_;
  VaultHost host_;
  std::string thumb_dir_;
  gfx::TextureCache thumb_tex_;
  std::future<JobResult> job_;
  std::string job_label_;
  std::function<void(const JobResult&)> job_then_;

  std::vector<vault::Item> items_;
  std::vector<vaultp::ItemStatus> status_;
  std::map<std::string, std::string> thumbs_;
  std::string selected_;
  vault::Item detail_;
  int pull_version_ = 0;  // 0 = latest
  std::string status_line_;
  std::string error_;
  bool loaded_ = false;

  // Filters.
  char search_[96] = "";
  int kind_filter_ = 0;  // 0 all, textures, models, audio, fonts, prefabs
  int dim_filter_ = 0;   // 0 any, 2d, 3d, vr
  char tag_[48] = "";
  float tile_size_ = 84.0f;

  // Settings.
  std::string backend_ = "local";
  char local_dir_[512] = "";
  char url_[512] = "";
  char key_[256] = "";
  std::string username_;
  bool open_settings_ = false;

  // Upload dialog.
  bool open_upload_ = false;
  std::string up_rel_;
  int up_source_ = 0;  // 0 Asset Browser selection, 1 prefab
  int up_prefab_ = 0;
  char up_name_[128] = "";
  char up_author_[96] = "";
  int up_licence_ = 0;
  char up_licence_custom_[96] = "";
  char up_tags_[256] = "";
  char up_desc_[1024] = "";
  char up_note_[256] = "";
  char up_thumb_[512] = "";
  bool up_dims_[3] = {false, false, false};
  std::vector<vaultp::Package> up_plan_;
  std::string up_plan_error_;

  // Vault manager: loose files, editor, deletes, claim picker.
  bool up_files_mode_ = false;
  std::vector<std::string> up_files_;
  char up_path_[1024] = "";
  bool editing_ = false;
  char ed_name_[128] = "";
  char ed_author_[96] = "";
  int ed_licence_ = 0;
  char ed_licence_custom_[96] = "";
  char ed_tags_[256] = "";
  char ed_desc_[1024] = "";
  bool ed_dims_[3] = {false, false, false};
  bool open_delete_ = false;
  std::string del_item_;
  std::string del_name_;
  int del_version_ = 0;  // 0 = the whole item
  int claim_pick_ = -1;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
