#pragma once

#include "editor/AppState.h"
#include "editor/ProjectInfo.h"
#include "editor/projects/ProjectStore.h"
#include "editor/screens/IScreen.h"
#include "editor/settings/Settings.h"
#include "editor/templates/TemplateStore.h"
#include "editor/workspace/Workspace2D.h"

#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

class Editor2DScreen;
class Editor3DScreen;

// Admin UI state machine:
//   Loading -> Login -> ProjectManager -> Editor2D
//                    \-> Settings <-> (Login | ProjectManager)
// Owns settings + on-disk project store; admin main drives ticks/actions.
class AppFlow {
 public:
  AppFlow();
  ~AppFlow();

  AppFlow(const AppFlow&) = delete;
  AppFlow& operator=(const AppFlow&) = delete;

  void start();  // loads settings, refreshes projects, enters Loading
  void tick(float delta_seconds);

  AppState current_state() const { return state_; }
  bool is_running() const { return state_ != AppState::Quit; }

  const Settings& settings() const { return settings_; }
  const std::string& last_error() const { return last_error_; }
  const std::string& status_message() const { return status_message_; }

  // Login helpers (no-ops unless current state is Login).
  bool try_login(const std::string& username, const std::string& password);
  bool submit_dev_login();  // DEV_LOGIN bypass -- see LoginScreen
  bool open_settings_from_login();

  // ProjectManager helpers.
  bool select_project(std::size_t index);  // Open
  bool create_new_project_2d(const std::string& name = "New 2D Project");
  // Any kind; starts with the default plugins for that kind.
  bool create_new_project(const std::string& name, ProjectKind kind);
  bool delete_project(std::size_t index);
  // Templates (Break Ground picker): create from a template id; vr = -1
  // template default, 0 off, 1 on (3D only).
  bool create_from_template(const std::string& name, const std::string& template_id, int vr = -1);
  bool save_project_as_template(std::size_t index, const std::string& name,
                                const std::string& description);
  // Claim Settings: VR switch on a 3D project.
  bool set_project_vr(std::size_t index, bool on);
  const TemplateStore& templates() const { return templates_; }
  void reload_templates();
  bool open_settings_from_projects();
  bool logout();

  // Settings helpers.
  bool settings_set_projects_root(std::string path);
  bool settings_set_username(std::string username);
  bool settings_set_auto_login_dev(bool enabled);
  bool settings_set_theme(std::string theme);
  bool apply_settings_draft();  // persist + reload projects if root changed
  bool cancel_settings();

  // Editor2D helpers.
  void request_back_to_projects();
  void request_quit();
  Workspace2D* editor_workspace();
  const Workspace2D* editor_workspace() const;
  bool editor_save_scene(std::string* error_out = nullptr);
  // Editor tools routed through Editor2DScreen (autosave included).
  // Return the number of entities affected (0 when not in Editor2D).
  std::size_t editor_duplicate_selected();
  std::size_t editor_delete_selected();
  // Undo / redo through the screen (autosaves scene.json). False when there
  // is nothing to undo/redo or not in Editor2D.
  bool editor_undo();
  bool editor_redo();
  // Active Editor2D screen (nullptr otherwise); lets --smoke drive the same
  // edit paths as the GUI.
  Editor2DScreen* editor_screen();
  // Active 3D editor (3D and VR claims; nullptr otherwise).
  Editor3DScreen* editor3d_screen();

  const std::vector<ProjectInfo>& projects() const {
    return project_store_.projects();
  }

  const ProjectInfo* active_project() const { return active_project_.get(); }

 private:
  void transition_to(AppState next);
  std::unique_ptr<IScreen> make_screen(AppState state);
  bool reload_projects();
  void persist_settings();
  void handle_pending_screen_actions();
  // Returns false if apply failed (caller must stay on Settings).
  bool try_apply_settings_from_screen();

  AppState state_ = AppState::Quit;
  AppState settings_return_state_ = AppState::ProjectManager;
  std::unique_ptr<IScreen> screen_;
  Settings settings_;
  ProjectStore project_store_;
  TemplateStore templates_;
  std::unique_ptr<ProjectInfo> active_project_;
  std::string status_message_;
  std::string last_error_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
