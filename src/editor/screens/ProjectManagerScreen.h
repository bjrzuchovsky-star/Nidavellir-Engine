#pragma once

#include "editor/ProjectInfo.h"
#include "editor/screens/IScreen.h"
#include "editor/store/TradingPostPanel.h"
#include "editor/update/UpdateBanner.h"
#include "editor/vault/VaultPanel.h"
#include "gfx/TextureCache.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ImVec2;

namespace ts {
namespace tombstone {
namespace editor {

class TemplateStore;
struct ProjectTemplate;

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

  // --- Templates (ProjectManager_Templates.cpp) ------------------------------
  // The picker lists `templates` (owned by AppFlow; may be null).
  void set_templates(const TemplateStore* templates) { templates_ = templates; }
  void open_template_picker(std::string preselect_id = {});
  bool template_picker_open() const { return picker_open_; }
  // vr: -1 = template default, 0 off, 1 on (3D only).
  // dimension: -1 = the template's own, 0 = 2D, 1 = 3D (Truly Blank).
  void request_new_from_template(std::string name, std::string template_id, int vr = -1,
                                 int dimension = -1);
  bool template_create_requested() const { return template_create_requested_; }
  const std::string& pending_template_id() const { return pending_template_id_; }
  int pending_template_vr() const { return pending_template_vr_; }
  int pending_template_dimension() const { return pending_template_dim_; }
  // Picker state for tools / screenshots (--templates-shot).
  void pick_template(const std::string& id);
  void clear_template_create_request();
  // Save the highlighted project as a user template.
  void request_save_as_template(std::string name, std::string description);
  bool save_template_requested() const { return save_template_requested_; }
  const std::string& pending_template_name() const { return pending_template_name_; }
  const std::string& pending_template_description() const { return pending_template_desc_; }
  void clear_save_template_request() { save_template_requested_ = false; }
  // Trading Post (plugin store), outfitting the highlighted claim.
  void open_trading_post() { trading_post_.open = true; }
  TradingPostUi& trading_post() { return trading_post_; }
  // The Vault manager: the library without opening a claim (browse, edit,
  // delete, Add files..., Pull into claim...). Drawn in place of the claims.
  void open_vault();
  void close_vault() { vault_open_ = false; }
  bool vault_open() const { return vault_open_; }
  VaultPanel* vault_panel() { return vault_.get(); }

  // Claim settings: VR switch on the highlighted 3D project.
  void request_set_vr(bool on);
  bool set_vr_requested() const { return set_vr_requested_; }
  bool pending_vr() const { return pending_vr_; }
  void clear_set_vr_request() { set_vr_requested_ = false; }

  // Update notice: AppFlow sets it each frame and takes the button press.
  void set_update_banner(UpdateBanner b) { update_banner_ = std::move(b); }
  const UpdateBanner& update_banner() const { return update_banner_; }
  void request_update_action(UpdateAction a) { update_action_ = a; }
  UpdateAction take_update_action() {
    const UpdateAction a = update_action_;
    update_action_ = UpdateAction::None;
    return a;
  }

 private:
  void draw_update_banner();
  void draw_vault_screen();
  void draw_ui();
  void draw_project_card(std::size_t index);
  void draw_kind_picker();
  void draw_template_picker();
  void draw_template_card(const ProjectTemplate& t, float w, float h);
  void draw_save_template_popup();
  void draw_claim_settings_popup();
  void draw_template_thumb(const ProjectTemplate& t, const ImVec2& p0, const ImVec2& p1);
  void draw_template_group(const char* title, const char* caption, const std::string& group);
  std::string default_claim_name(const ProjectTemplate& t) const;

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

  // Templates.
  const TemplateStore* templates_ = nullptr;
  gfx::TextureCache thumbs_;
  bool picker_open_ = false;
  bool picker_popup_pending_ = false;
  std::string picked_template_id_;
  int picker_filter_ = 0;  // 0 all, 1 2D, 2 3D, 3 VR, 4 mine
  bool picker_vr_ = false;
  char picker_search_[64]{};
  char picker_name_buf_[128]{};
  bool template_create_requested_ = false;
  std::string pending_template_id_;
  int pending_template_vr_ = -1;
  int pending_template_dim_ = -1;
  int picker_dim_ = -1;  // Truly Blank's 2D / 3D switch: 0 / 1, -1 = template's
  bool save_popup_pending_ = false;
  char save_name_buf_[128]{};
  char save_desc_buf_[256]{};
  bool save_template_requested_ = false;
  std::string pending_template_name_;
  std::string pending_template_desc_;
  bool settings_popup_pending_ = false;
  TradingPostUi trading_post_;
  bool set_vr_requested_ = false;
  bool pending_vr_ = false;
  UpdateBanner update_banner_;
  UpdateAction update_action_ = UpdateAction::None;
  std::unique_ptr<VaultPanel> vault_;
  bool vault_open_ = false;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
