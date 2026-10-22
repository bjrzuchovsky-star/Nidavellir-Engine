#pragma once

#include "editor/screens/IScreen.h"
#include "editor/settings/Settings.h"
#include "editor/ui/FolderBrowser.h"

#include <string>

namespace ts {
namespace tombstone {
namespace editor {

// ImGui settings editor. Reachable from ProjectManager or Login.
// Applying a new projects_root asks AppFlow to reload the project list.
class SettingsScreen final : public IScreen {
 public:
  explicit SettingsScreen(Settings settings, AppState return_state);

  AppState state() const override { return AppState::Settings; }

  void on_enter() override;
  void on_exit() override;
  AppState on_update(float delta_seconds) override;

  const Settings& draft() const { return draft_; }
  AppState return_state() const { return return_state_; }

  void set_projects_root(std::string path);
  void set_username(std::string username);
  void set_auto_login_dev(bool enabled);
  void set_theme(std::string theme);

  // Marks draft ready to apply and leave (AppFlow persists + reloads).
  // Performs local validation first; on failure sets validation_error_ and
  // does not set apply_requested_.
  void request_apply();
  void request_cancel();

  bool apply_requested() const { return apply_requested_; }
  bool cancel_requested() const { return cancel_requested_; }

  void clear_apply_request();
  void set_validation_error(std::string message);
  const std::string& validation_error() const { return validation_error_; }

 private:
  void draw_ui();
  void sync_buffers_from_draft();
  void sync_draft_from_buffers();

  Settings draft_;
  AppState return_state_ = AppState::ProjectManager;
  bool apply_requested_ = false;
  bool cancel_requested_ = false;
  std::string validation_error_;
  char projects_root_buf_[512]{};
  char username_buf_[128]{};
  char blender_buf_[512]{};
  FolderBrowser blender_browser_;
  bool blender_probed_ = false;   // auto-detect once per visit (it walks the disk)
  std::string blender_found_;
  int theme_index_ = 0;  // 0=dark, 1=light
  FolderBrowser folder_browser_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
