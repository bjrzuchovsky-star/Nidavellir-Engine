#include "editor/screens/LoginScreen.h"
#include "editor/ux/Tooltip.h"

#include "editor/ui/Brand.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <iostream>

namespace ts {
namespace tombstone {
namespace editor {

LoginScreen::LoginScreen(std::string last_username, bool auto_login_dev)
    : auto_login_dev_(auto_login_dev),
      last_username_(std::move(last_username)) {}

void LoginScreen::on_enter() {
  authenticated_ = false;
  settings_requested_ = false;
  auto_login_attempted_ = false;
  username_ = last_username_;
  error_message_.clear();
  std::memset(username_buf_, 0, sizeof(username_buf_));
  std::memset(password_buf_, 0, sizeof(password_buf_));
  if (!last_username_.empty()) {
    std::strncpy(username_buf_, last_username_.c_str(),
                 sizeof(username_buf_) - 1);
  }
  std::cout << "[Login] enter credentials, DEV_LOGIN, or open Settings\n";
}

void LoginScreen::on_exit() {
  if (authenticated_) {
    std::cout << "[Login] authenticated as \"" << username_ << "\"\n";
  }
}

void LoginScreen::draw_ui() {
  theme::BeginRoot("##LoginRoot");

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float panel_w = 440.0f;
  const float panel_h = 420.0f;
  ImGui::SetCursorPos(ImVec2(std::max(0.0f, (avail.x - panel_w) * 0.5f),
                             std::max(0.0f, (avail.y - panel_h) * 0.5f)));
  theme::BeginCard("LoginPanel", panel_w, panel_h);

  const float inner_w = ImGui::GetContentRegionAvail().x;
  theme::BrandBlock(brand::kAdminTitle, "Sign the ledger. Ride in.", inner_w);
  ImGui::Dummy(ImVec2(0, 6.0f));
  theme::SectionHeader("Credentials", "who goes there");

  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##user", "Rider name", username_buf_,
                           sizeof(username_buf_));
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##pass", "Password", password_buf_,
                           sizeof(password_buf_),
                           ImGuiInputTextFlags_Password |
                               ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsItemDeactivatedAfterEdit() &&
      ImGui::IsKeyPressed(ImGuiKey_Enter)) {
    try_login(username_buf_, password_buf_);
  }

  if (!error_message_.empty()) {
    ImGui::Spacing();
    theme::StatusError(error_message_.c_str());
  }

  ImGui::Dummy(ImVec2(0, 8.0f));
  if (theme::PrimaryButton("Ride In", ImVec2(-1, 34.0f))) {
    try_login(username_buf_, password_buf_);
  }
  const float half_w = (inner_w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  if (theme::CopperButton("Dev Login", ImVec2(half_w, 30.0f))) {
    submit_dev_login();
  }
  if (ImGui::IsItemHovered()) {
    ux::SetTooltip("Skip the gate for local work (signs in as the saved user).");
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Settings", ImVec2(half_w, 30.0f))) {
    request_settings();
  }

  ImGui::Dummy(ImVec2(0, 8.0f));
  theme::StatusInfo(
      "Stub auth for now: any name and password gets through. "
      "Dev Login skips the line.");

  theme::EndCard();
  theme::EndRoot();
}

AppState LoginScreen::on_update(float /*delta_seconds*/) {
  if (auto_login_dev_ && !auto_login_attempted_ && !authenticated_ &&
      !settings_requested_) {
    auto_login_attempted_ = true;
    submit_dev_login();
  }

  if (ImGui::GetCurrentContext() != nullptr) {
    draw_ui();
  }

  if (authenticated_) {
    return AppState::ProjectManager;
  }
  if (settings_requested_) {
    return AppState::Settings;
  }
  return AppState::Login;
}

bool LoginScreen::try_login(const std::string& username,
                            const std::string& password) {
  if (username.empty() || password.empty()) {
    error_message_ = "Need a name and a password. Both.";
    std::cout << "[Login] rejected: " << error_message_ << '\n';
    return false;
  }
  username_ = username;
  authenticated_ = true;
  settings_requested_ = false;
  error_message_.clear();
  std::cout << "[Login] success (stub credential check)\n";
  return true;
}

bool LoginScreen::submit_dev_login() {
  username_ = last_username_.empty() ? "dev" : last_username_;
  authenticated_ = true;
  settings_requested_ = false;
  error_message_.clear();
  std::cout << "[Login] DEV_LOGIN bypass accepted as \"" << username_
            << "\"\n";
  return true;
}

void LoginScreen::request_settings() {
  settings_requested_ = true;
  std::cout << "[Login] Settings requested\n";
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
