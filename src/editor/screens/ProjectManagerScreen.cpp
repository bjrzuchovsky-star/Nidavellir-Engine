#include "editor/screens/ProjectManagerScreen.h"

#include "editor/projects/ProjectStore.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace ts {
namespace tombstone {
namespace editor {

ProjectManagerScreen::ProjectManagerScreen(std::vector<ProjectInfo> projects,
                                           std::string projects_root,
                                           std::string status_message)
    : projects_(std::move(projects)),
      projects_root_(std::move(projects_root)),
      status_message_(std::move(status_message)) {}

void ProjectManagerScreen::on_enter() {
  selected_index_.reset();
  open_editor_2d_ = false;
  new_project_requested_ = false;
  pending_new_project_name_.clear();
  delete_requested_ = false;
  pending_delete_index_.reset();
  confirm_delete_open_ = false;
  settings_requested_ = false;
  logout_requested_ = false;
  error_message_.clear();
  std::memset(new_name_buf_, 0, sizeof(new_name_buf_));
  new_kind_ = ProjectKind::TwoD;
  pending_new_kind_ = ProjectKind::TwoD;
  std::strncpy(new_name_buf_, "New 2D Project", sizeof(new_name_buf_) - 1);
  picker_open_ = false;
  picker_popup_pending_ = false;
  template_create_requested_ = false;
  save_template_requested_ = false;
  set_vr_requested_ = false;

  std::cout << "[ProjectManager] root=" << projects_root_ << '\n';
  if (!status_message_.empty()) {
    std::cout << "[ProjectManager] " << status_message_ << '\n';
  }
}

void ProjectManagerScreen::on_exit() {
  if (const ProjectInfo* p = selected_project()) {
    std::cout << "[ProjectManager] opening \"" << p->name << "\"\n";
  }
}

void ProjectManagerScreen::draw_project_card(std::size_t i) {
  const ProjectInfo& p = projects_[i];
  const bool highlight =
      highlighted_index_.has_value() && *highlighted_index_ == i;

  ImGui::PushID(static_cast<int>(i));
  const ImVec4 base = theme::PanelBg();
  const ImVec4 lit = theme::Accent();
  ImGui::PushStyleColor(
      ImGuiCol_ChildBg,
      highlight ? ImVec4(base.x + (lit.x - base.x) * 0.14f,
                         base.y + (lit.y - base.y) * 0.14f,
                         base.z + (lit.z - base.z) * 0.14f, 1.0f)
                : base);
  ImGui::PushStyleColor(ImGuiCol_Border,
                        highlight ? theme::AccentMuted() : theme::Border());
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::metrics::kRadiusCard);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 9.0f));
  ImGui::BeginChild("card", ImVec2(0, 80), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar);

  // Left rail: amber when picked, stone otherwise.
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float h = ImGui::GetWindowSize().y;
    dl->AddRectFilled(wp, ImVec2(wp.x + 4.0f, wp.y + h),
                      theme::U32(highlight ? theme::Accent() : theme::Border()),
                      theme::metrics::kRadiusCard, ImDrawFlags_RoundCornersLeft);
  }

  ImGui::BeginGroup();
  ImGui::PushStyleColor(ImGuiCol_Text,
                        highlight ? theme::Accent() : theme::Text());
  ImGui::TextUnformatted(p.name.c_str());
  ImGui::PopStyleColor();
  ImGui::SameLine();
  theme::DimensionBadge(p.kind == ProjectKind::TwoD ? "2D" : "3D");
  if (vr_enabled(p)) {
    ImGui::SameLine();
    theme::DimensionBadge("VR");
  }
  ImGui::TextDisabled("%s", p.path.c_str());
  if (!p.last_opened.empty()) {
    ImGui::TextDisabled("Last worked  %s", p.last_opened.c_str());
  } else if (!p.created.empty()) {
    ImGui::TextDisabled("Staked  %s", p.created.c_str());
  }
  ImGui::EndGroup();

  if (highlight) {
    const char* hint = vr_enabled(p)
                           ? "Opens the 3D editor; Preview in headset to ride it in VR"
                           : "Double-click or Open to ride in";
    const ImVec2 hs = ImGui::CalcTextSize(hint);
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - hs.x - 16.0f,
                               ImGui::GetWindowHeight() - hs.y - 9.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::CopperMuted());
    ImGui::TextUnformatted(hint);
    ImGui::PopStyleColor();
  }

  if (ImGui::IsWindowHovered() &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    highlighted_index_ = i;
    error_message_.clear();
  }
  if (ImGui::IsWindowHovered() &&
      ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    select_project(i);
  }

  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  ImGui::PopID();
  ImGui::Dummy(ImVec2(0, 2.0f));
}

void ProjectManagerScreen::draw_update_banner() {
  const UpdateBanner& b = update_banner_;
  if (!b.visible) return;
  ImGui::PushID("update_banner");
  const ImVec4 base = theme::PanelBg();
  const ImVec4 lit = theme::Accent();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(base.x + (lit.x - base.x) * 0.10f,
                                                 base.y + (lit.y - base.y) * 0.10f,
                                                 base.z + (lit.z - base.z) * 0.10f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, theme::AccentMuted());
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::metrics::kRadiusCard);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 8.0f));
  const float h = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y + 16.0f +
                  (b.error.empty() ? 0.0f : ImGui::GetTextLineHeightWithSpacing());
  ImGui::BeginChild("update", ImVec2(0, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    dl->AddRectFilled(wp, ImVec2(wp.x + 4.0f, wp.y + ImGui::GetWindowSize().y), theme::U32(theme::Accent()),
                      theme::metrics::kRadiusCard, ImDrawFlags_RoundCornersLeft);
  }
  ImGui::AlignTextToFramePadding();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
  ImGui::Text("New update available: v%s", b.version.c_str());
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("You're riding v%s.  Fresh supplies are in from the trail.", b.current.c_str());
  if (b.fake && ImGui::IsItemHovered()) ImGui::SetTooltip("Test notice (TS_FAKE_UPDATE); nothing to download.");

  if (b.busy) {
    const float frac = b.progress >= 0.0 ? static_cast<float>(b.progress) : 0.0f;
    theme::BrandProgress(frac, ImVec2(220.0f, ImGui::GetFrameHeight()));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", b.note.empty() ? "Fetching the update..." : b.note.c_str());
  } else {
    if (theme::PrimaryButton("Update", ImVec2(96, 0))) update_action_ = UpdateAction::Update;
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Downloads the installer, checks it against SHA256SUMS.txt, runs it and closes the editor.\n"
                        "Your claims, settings and Vault stay put.");
    }
    ImGui::SameLine();
    if (theme::CopperButton("Release notes", ImVec2(130, 0))) update_action_ = UpdateAction::ReleaseNotes;
    ImGui::SameLine();
    if (theme::SecondaryButton("Later", ImVec2(80, 0))) update_action_ = UpdateAction::Later;
    ImGui::SameLine();
    if (theme::SecondaryButton("Skip this version", ImVec2(150, 0))) update_action_ = UpdateAction::Skip;
  }
  if (!b.error.empty()) {
    theme::StatusError(b.error.c_str());
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  ImGui::PopID();
}

void ProjectManagerScreen::draw_ui() {
  theme::BeginRoot("##ProjectManagerRoot");

  theme::TitleStrip("Project Manager", "Your claims on the frontier");
  draw_update_banner();

  char caption[64];
  std::snprintf(caption, sizeof(caption), "%zu on file", projects_.size());
  theme::SectionHeader("Claims", caption);
  ImGui::TextDisabled("Territory: %s", projects_root_.c_str());

  if (!status_message_.empty()) {
    theme::StatusSuccess(status_message_.c_str());
  }
  if (!error_message_.empty()) {
    theme::StatusError(error_message_.c_str());
  }
  ImGui::Spacing();

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::BeginChild("ProjectList", ImVec2(0, -96), ImGuiChildFlags_None);
  if (projects_.empty()) {
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0, 36));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c = ImGui::GetCursorScreenPos();
    theme::TombstoneMark(dl, ImVec2(c.x + w * 0.5f, c.y), 56.0f,
                         theme::U32(theme::Border()),
                         theme::U32(theme::Charcoal(), 0.7f));
    ImGui::Dummy(ImVec2(w, 68.0f));
    theme::CenteredText("Nothing staked yet.", 22.0f, theme::Text(), w);
    ImGui::Dummy(ImVec2(0, 4));
    theme::CenteredText("Break ground below: pick a template (blank 2D / 3D or a sample),",
                        ImGui::GetFontSize(), theme::TextMuted(), w);
    theme::CenteredText("name your claim and ride in.",
                        ImGui::GetFontSize(), theme::TextMuted(), w);
  } else {
    for (std::size_t i = 0; i < projects_.size(); ++i) {
      draw_project_card(i);
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  theme::SectionHeader("Break Ground", "new claim from a template: blank 2D / 3D or a sample");
  if (theme::PrimaryButton("+ New Claim...", ImVec2(140, 0))) {
    open_template_picker();
  }
  ImGui::SameLine();
  if (theme::CopperButton("Open", ImVec2(80, 0))) {
    if (!highlighted_index_.has_value()) {
      error_message_ = "Pick a claim first, then Open.";
    } else {
      select_project(*highlighted_index_);
    }
  }
  ImGui::SameLine();
  if (theme::DangerButton("Delete", ImVec2(80, 0))) {
    if (!highlighted_index_.has_value()) {
      error_message_ = "Pick a claim first, then Delete.";
    } else {
      confirm_delete_open_ = true;
      ImGui::OpenPopup("Bury Project?");
    }
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Save as Template", ImVec2(140, 0))) {
    if (!highlighted_index_.has_value()) {
      error_message_ = "Pick a claim first, then Save as Template.";
    } else {
      const ProjectInfo& hp = projects_[*highlighted_index_];
      std::memset(save_name_buf_, 0, sizeof(save_name_buf_));
      std::memset(save_desc_buf_, 0, sizeof(save_desc_buf_));
      std::snprintf(save_name_buf_, sizeof(save_name_buf_), "%s Template", hp.name.c_str());
      save_popup_pending_ = true;
    }
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Claim Settings", ImVec2(130, 0))) {
    if (!highlighted_index_.has_value()) {
      error_message_ = "Pick a claim first, then Claim Settings.";
    } else {
      settings_popup_pending_ = true;
    }
  }
  ImGui::SameLine();
  if (theme::CopperButton("Trading Post", ImVec2(120, 0))) {
    open_trading_post();
  }
  ImGui::SameLine(0.0f, 24.0f);
  if (theme::SecondaryButton("Settings", ImVec2(90, 0))) {
    request_settings();
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Log Out", ImVec2(80, 0))) {
    request_logout();
  }

  if (confirm_delete_open_) {
    ImGui::OpenPopup("Bury Project?");
  }
  if (ImGui::BeginPopupModal("Bury Project?", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    const char* name = "(none)";
    if (highlighted_index_.has_value() &&
        *highlighted_index_ < projects_.size()) {
      name = projects_[*highlighted_index_].name.c_str();
    }
    ImGui::Text("Bury \"%s\" for good?", name);
    theme::StatusWarn("This deletes the project folder from disk. No coming back.");
    ImGui::Spacing();
    if (theme::DangerButton("Bury It", ImVec2(120, 0))) {
      request_delete_highlighted();
      confirm_delete_open_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (theme::SecondaryButton("Keep It", ImVec2(120, 0))) {
      confirm_delete_open_ = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  draw_template_picker();
  draw_save_template_popup();
  draw_claim_settings_popup();
  {
    TradingPostTarget target;
    if (highlighted_index_ && *highlighted_index_ < projects_.size()) {
      target.project = &projects_[*highlighted_index_];
    }
    draw_trading_post_window(trading_post_, target);
  }

  theme::EndRoot();
}

AppState ProjectManagerScreen::on_update(float /*delta_seconds*/) {
  if (ImGui::GetCurrentContext() != nullptr) {
    draw_ui();
  }

  if (open_editor_2d_) {
    const ProjectInfo* p = selected_project();
    return p && p->kind != ProjectKind::TwoD ? AppState::Editor3D : AppState::Editor2D;
  }
  if (settings_requested_) {
    return AppState::Settings;
  }
  if (logout_requested_) {
    return AppState::Login;
  }
  return AppState::ProjectManager;
}

void ProjectManagerScreen::set_projects(std::vector<ProjectInfo> projects) {
  projects_ = std::move(projects);
}

void ProjectManagerScreen::set_status_message(std::string message) {
  status_message_ = std::move(message);
}

void ProjectManagerScreen::set_error_message(std::string message) {
  error_message_ = std::move(message);
}

bool ProjectManagerScreen::select_project(std::size_t index) {
  if (index >= projects_.size()) {
    error_message_ = "Invalid project index " + std::to_string(index) + ".";
    std::cout << "[ProjectManager] " << error_message_ << '\n';
    return false;
  }

  const ProjectInfo& project = projects_[index];
  highlighted_index_ = index;
  selected_index_ = index;
  open_editor_2d_ = true;
  error_message_.clear();
  std::cout << "[ProjectManager] selected " << to_string(project.kind)
            << " project \"" << project.name << "\"\n";
  return true;
}

bool ProjectManagerScreen::highlight_project_by_id(const std::string& id) {
  for (std::size_t i = 0; i < projects_.size(); ++i) {
    if (projects_[i].id == id) {
      highlighted_index_ = i;
      return true;
    }
  }
  return false;
}

const ProjectInfo* ProjectManagerScreen::selected_project() const {
  if (!selected_index_.has_value()) {
    return nullptr;
  }
  return &projects_[*selected_index_];
}

void ProjectManagerScreen::request_new_project_2d(std::string name) {
  request_new_project(std::move(name), ProjectKind::TwoD);
}

void ProjectManagerScreen::request_new_project(std::string name,
                                               ProjectKind kind) {
  pending_new_project_name_ = std::move(name);
  pending_new_kind_ = kind;
  new_project_requested_ = true;
  std::cout << "[ProjectManager] New " << to_string(kind)
            << " Project requested: \"" << pending_new_project_name_
            << "\"\n";
}

void ProjectManagerScreen::set_new_project_kind(ProjectKind kind) {
  // Keep a default name in step with the type ("New 3D Project").
  const std::string current = new_name_buf_;
  const std::string old_default =
      std::string("New ") + to_string(new_kind_) + " Project";
  new_kind_ = kind;
  if (current.empty() || current == old_default) {
    const std::string fresh = std::string("New ") + to_string(kind) + " Project";
    std::memset(new_name_buf_, 0, sizeof(new_name_buf_));
    std::strncpy(new_name_buf_, fresh.c_str(), sizeof(new_name_buf_) - 1);
  }
}

void ProjectManagerScreen::draw_kind_picker() {
  struct Choice {
    ProjectKind kind;
    const char* title;
    const char* line1;
    const char* line2;
  };
  static const Choice kChoices[] = {
      {ProjectKind::TwoD, "2D", "Frontier town", "Ready to ride today"},
      {ProjectKind::ThreeD, "3D", "Open range", "GL 3.3, glTF, PBR"},
      {ProjectKind::VR, "VR", "Through the goggles", "OpenXR + desktop simulator"},
  };
  // Wide enough for the longest line of copy at the current font.
  float w = 200.0f;
  for (const Choice& c : kChoices) {
    w = std::max(w, 64.0f + 16.0f +
                        std::max(ImGui::CalcTextSize(c.line1).x,
                                 ImGui::CalcTextSize(c.line2).x));
  }
  const float h = 58.0f;
  for (int i = 0; i < 3; ++i) {
    const Choice& c = kChoices[i];
    const bool on = new_kind_ == c.kind;
    if (i > 0) ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushID(i);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("kind", ImVec2(w, h))) {
      set_new_project_kind(c.kind);
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + w, p0.y + h);
    const ImVec4 base = theme::PanelBg();
    const ImVec4 lit = theme::Accent();
    const float mix = on ? 0.16f : (hovered ? 0.07f : 0.0f);
    const ImVec4 fill(base.x + (lit.x - base.x) * mix,
                      base.y + (lit.y - base.y) * mix,
                      base.z + (lit.z - base.z) * mix, 1.0f);
    dl->AddRectFilled(p0, p1, theme::U32(fill), theme::metrics::kRadiusCard);
    dl->AddRect(p0, p1, theme::U32(on ? theme::Accent() : theme::Border()),
                theme::metrics::kRadiusCard, 0, on ? 2.0f : 1.0f);
    // Big type letters on the left, two lines of copy on the right.
    theme::DrawSizedText(dl, ImVec2(p0.x + 12.0f, p0.y + 12.0f), 30.0f,
                         theme::U32(on ? theme::Accent() : theme::Sand()),
                         c.title);
    dl->AddText(ImVec2(p0.x + 64.0f, p0.y + 10.0f),
                theme::U32(on ? theme::Text() : theme::TextMuted()), c.line1);
    dl->AddText(ImVec2(p0.x + 64.0f, p0.y + 31.0f),
                theme::U32(theme::Success()), c.line2);
    if (hovered) {
      ImGui::SetTooltip("%s", c.kind == ProjectKind::TwoD
                                  ? "The full 2D editor: tiles, sprites, Play, posse."
                              : c.kind == ProjectKind::ThreeD
                                  ? "The 3D editor: glTF, PBR, Lua, Play."
                                  : "The 3D editor with a VR rig: grab, throw, teleport. "
                                    "Preview in headset (OpenXR / SteamVR) or the "
                                    "desktop simulator.");
    }
    ImGui::PopID();
  }
}

void ProjectManagerScreen::request_delete_highlighted() {
  if (!highlighted_index_.has_value()) {
    error_message_ = "Select a project before Delete.";
    return;
  }
  pending_delete_index_ = highlighted_index_;
  delete_requested_ = true;
  std::cout << "[ProjectManager] Delete requested index="
            << *pending_delete_index_ << '\n';
}

void ProjectManagerScreen::request_settings() {
  settings_requested_ = true;
  std::cout << "[ProjectManager] Settings requested\n";
}

void ProjectManagerScreen::request_logout() {
  logout_requested_ = true;
  std::cout << "[ProjectManager] Logout requested\n";
}

void ProjectManagerScreen::clear_new_project_request() {
  new_project_requested_ = false;
  pending_new_project_name_.clear();
}

void ProjectManagerScreen::clear_delete_request() {
  delete_requested_ = false;
  pending_delete_index_.reset();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
