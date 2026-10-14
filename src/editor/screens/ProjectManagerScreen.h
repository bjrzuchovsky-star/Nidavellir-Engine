#pragma once

#include "editor/ProjectInfo.h"
#include "editor/screens/IScreen.h"

#include <optional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

// On-disk project list/select shell with ImGui UI.
// Actions: Open (select index / double-click), New Project (2D / 3D / VR
// picker), Delete, Settings, Logout. A 2D project opens Editor2D; 3D and
// VR open Editor3D (VR claims add the headset preview).
class ProjectManagerScreen final : public IScreen {
 public:
  ProjectManagerScreen(std::vector<ProjectInfo> projects,
                       std::string projects_root,
                       std::string status_message = {});

  AppState state() const override { return AppState::ProjectManager; }

  void on_enter() override;
  void on_exit() override;
  AppState on_update(float delta_seconds) override;

  const std::vector<ProjectInfo>& projects() const { return projects_; }
  void set_projects(std::vector<ProjectInfo> projects);
  void set_status_message(std::string message);
  void set_error_message(std::string message);

  // Index into projects(). Returns false for out-of-range or unsupported kinds.
  bool select_project(std::size_t index);

  // Highlight a project in the list without opening the editor (e.g. after create).
  bool highlight_project_by_id(const std::string& id);

  const ProjectInfo* selected_project() const;
  std::optional<std::size_t> highlighted_index() const {
    return highlighted_index_;
  }

  void request_new_project_2d(std::string name = "New 2D Project");
  void request_new_project(std::string name, ProjectKind kind);
  // Project-type picker (Break Ground).
  ProjectKind new_project_kind() const { return new_kind_; }
  void set_new_project_kind(ProjectKind kind);
  ProjectKind pending_new_project_kind() const { return pending_new_kind_; }
  void request_delete_highlighted();
  void request_settings();
  void request_logout();

  bool new_project_requested() const { return new_project_requested_; }
  const std::string& pending_new_project_name() const {
    return pending_new_project_name_;
  }
  bool delete_requested() const { return delete_requested_; }
  std::optional<std::size_t> pending_delete_index() const {
    return pending_delete_index_;
  }
  bool settings_requested() const { return settings_requested_; }
  bool logout_requested() const { return logout_requested_; }

  // Called by AppFlow after it handles new-project / delete.
  void clear_new_project_request();
  void clear_delete_request();

 private:
  void draw_ui();
  void draw_project_card(std::size_t index);
  void draw_kind_picker();

  std::vector<ProjectInfo> projects_;
  std::string projects_root_;
  std::string status_message_;
  std::string error_message_;
  std::optional<std::size_t> highlighted_index_;
  std::optional<std::size_t> selected_index_;
  bool open_editor_2d_ = false;  // open the selected project (any kind)
  ProjectKind new_kind_ = ProjectKind::TwoD;
  ProjectKind pending_new_kind_ = ProjectKind::TwoD;
  bool new_project_requested_ = false;
  std::string pending_new_project_name_;
  bool delete_requested_ = false;
  std::optional<std::size_t> pending_delete_index_;
  bool confirm_delete_open_ = false;
  bool settings_requested_ = false;
  bool logout_requested_ = false;
  char new_name_buf_[128]{};
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
