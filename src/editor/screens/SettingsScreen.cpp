#include "editor/screens/SettingsScreen.h"

#include "editor/update/UpdateService.h"

#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <iostream>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

int theme_to_index(const std::string& theme) {
  return (theme == "light") ? 1 : 0;
}

const char* index_to_theme(int index) {
  return index == 1 ? "light" : "dark";
}

}  // namespace

SettingsScreen::SettingsScreen(Settings settings, AppState return_state)
    : draft_(std::move(settings)), return_state_(return_state) {}

void SettingsScreen::sync_buffers_from_draft() {
  std::memset(projects_root_buf_, 0, sizeof(projects_root_buf_));
  std::memset(username_buf_, 0, sizeof(username_buf_));
  std::strncpy(projects_root_buf_, draft_.projects_root.c_str(),
               sizeof(projects_root_buf_) - 1);
  std::strncpy(username_buf_, draft_.username.c_str(),
               sizeof(username_buf_) - 1);
  theme_index_ = theme_to_index(draft_.theme);
}

void SettingsScreen::sync_draft_from_buffers() {
  draft_.projects_root = projects_root_buf_;
  draft_.username = username_buf_;
  draft_.theme = index_to_theme(theme_index_);
}

void SettingsScreen::on_enter() {
  apply_requested_ = false;
  cancel_requested_ = false;
  validation_error_.clear();
  folder_browser_.close();
  sync_buffers_from_draft();
  std::cout << "[Settings] edit config\n";
}

void SettingsScreen::on_exit() {
  folder_browser_.close();
  std::cout << "[Settings] leaving\n";
}

void SettingsScreen::draw_ui() {
  theme::BeginRoot("##SettingsRoot");

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float panel_w = std::min(avail.x * 0.72f, 660.0f);
  const float panel_h = std::min(avail.y * 0.94f, 680.0f);
  ImGui::SetCursorPos(ImVec2((avail.x - panel_w) * 0.5f,
                             (avail.y - panel_h) * 0.5f));
  theme::BeginCard("##SettingsCard", panel_w, panel_h);

  theme::TitleStrip("Settings", "Set your camp");
  ImGui::TextDisabled("Ledger file: %s",
                      SettingsStore::default_settings_path().c_str());

  theme::SectionHeader("Territory", "where projects live");
  ImGui::SetNextItemWidth(-110.0f);
  ImGui::InputTextWithHint("##projects_root", "./TombStoneProjects",
                           projects_root_buf_, sizeof(projects_root_buf_));
  ImGui::SameLine();
  if (theme::SecondaryButton("Browse...", ImVec2(100, 0))) {
    validation_error_.clear();
    folder_browser_.open(projects_root_buf_);
  }
  theme::StatusInfo("Browse opens the in-app folder picker. Same on every OS.");

  if (folder_browser_.draw("Select projects_root folder")) {
    const std::string picked = folder_browser_.take_result();
    if (!picked.empty()) {
      set_projects_root(picked);
      std::cout << "[Settings] browsed projects_root -> " << picked << '\n';
    }
  }

  theme::SectionHeader("Rider", "account");
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##username", "Default rider name", username_buf_,
                           sizeof(username_buf_));
  ImGui::Checkbox("Ride in on Dev login at startup", &draft_.auto_login_dev);
  theme::StatusInfo("Skips the gate and signs in as the saved rider.");

  theme::SectionHeader("Lamplight", "appearance");
  const char* themes[] = {"Dusk  (charcoal + amber)",
                          "Parchment  (warm light)"};
  ImGui::SetNextItemWidth(280.0f);
  ImGui::Combo("##theme", &theme_index_, themes, 2);
  ImGui::SameLine();
  ImGui::TextDisabled("Theme");

  theme::SectionHeader("Supply Run", "engine updates");
  ImGui::Checkbox("Check for updates on start", &draft_.check_updates_on_start);
  if (update::url_supported(update::kLatestReleaseUrl)) {
    theme::StatusInfo("Asks GitHub for a newer release when the Project Manager opens (at most every 6 hours).");
  } else {
    theme::StatusInfo("Update checks need the Windows editor (https); this build skips them.");
  }
  if (!draft_.update_skip_version.empty()) {
    ImGui::TextDisabled("Skipping v%s", draft_.update_skip_version.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop skipping")) draft_.update_skip_version.clear();
  }

  if (!draft_.last_project_path.empty()) {
    theme::SectionHeader("Trail", "last session");
    ImGui::TextDisabled("Last project: %s", draft_.last_project_path.c_str());
  }

  if (!validation_error_.empty()) {
    ImGui::Spacing();
    theme::StatusError(validation_error_.c_str());
  }

  ImGui::Dummy(ImVec2(0, 10.0f));
  theme::Divider();
  if (theme::PrimaryButton("Save & Apply", ImVec2(160, 34.0f))) {
    request_apply();
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Cancel", ImVec2(120, 34.0f))) {
    request_cancel();
  }

  theme::EndCard();
  theme::EndRoot();
}

AppState SettingsScreen::on_update(float /*delta_seconds*/) {
  if (ImGui::GetCurrentContext() != nullptr) {
    draw_ui();
  }
  if (apply_requested_ || cancel_requested_) {
    return return_state_;
  }
  return AppState::Settings;
}

void SettingsScreen::set_projects_root(std::string path) {
  draft_.projects_root = std::move(path);
  std::memset(projects_root_buf_, 0, sizeof(projects_root_buf_));
  std::strncpy(projects_root_buf_, draft_.projects_root.c_str(),
               sizeof(projects_root_buf_) - 1);
}

void SettingsScreen::set_username(std::string username) {
  draft_.username = std::move(username);
  std::memset(username_buf_, 0, sizeof(username_buf_));
  std::strncpy(username_buf_, draft_.username.c_str(),
               sizeof(username_buf_) - 1);
}

void SettingsScreen::set_auto_login_dev(bool enabled) {
  draft_.auto_login_dev = enabled;
}

void SettingsScreen::set_theme(std::string theme) {
  draft_.theme = std::move(theme);
  theme_index_ = theme_to_index(draft_.theme);
}

void SettingsScreen::request_apply() {
  sync_draft_from_buffers();
  validation_error_.clear();

  if (draft_.projects_root.empty()) {
    validation_error_ =
        "Territory can't be blank. Try ./TombStoneProjects.";
    apply_requested_ = false;
    return;
  }

  apply_requested_ = true;
  cancel_requested_ = false;
  std::cout << "[Settings] apply requested\n";
}

void SettingsScreen::request_cancel() {
  cancel_requested_ = true;
  apply_requested_ = false;
  validation_error_.clear();
  std::cout << "[Settings] cancel requested\n";
}

void SettingsScreen::clear_apply_request() {
  apply_requested_ = false;
}

void SettingsScreen::set_validation_error(std::string message) {
  validation_error_ = std::move(message);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
