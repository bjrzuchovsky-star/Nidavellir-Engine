#include "editor/ux/EditorUx.h"
#include "editor/ux/FriendlyError.h"
#include "editor/ux/Layouts.h"
#include "editor/ui/Theme.h"

#include "editor/ux/UxPrefs.h"
#include "editor/ux/UxStrings.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>

namespace ts::tombstone::editor::ux {

namespace {
std::string g_settings_section;
bool g_settings_requested = false;
std::string g_open_claim;
}  // namespace

void request_settings_section(const std::string& section) {
  g_settings_section = section;
  g_settings_requested = true;
}
std::string take_settings_section() {
  g_settings_requested = false;
  std::string s;
  s.swap(g_settings_section);
  return s;
}
bool settings_section_requested() { return g_settings_requested; }
void request_open_claim(const std::string& path) { g_open_claim = path; }
std::string take_open_claim() {
  std::string s;
  s.swap(g_open_claim);
  return s;
}

EditorUx::EditorUx(bool three_d) : three_d_(three_d), walk_(three_d) {
  Action p;
  p.id = "Palette/Command Palette";
  p.label = "Command Palette";
  p.path = "Help";
  p.shortcut = "Ctrl+Shift+P";
  p.tip = "Search every menu item, panel, object type and setting by name.";
  p.run = [this] { palette_.open(); };
  actions_.add(std::move(p));
}

void EditorUx::add_panel(const std::string& label, const std::string& window, bool* shown) {
  Action a;
  a.id = "Panel/" + label;
  a.label = label;
  a.path = "Panel";
  a.tip = "Show the " + label + " panel and bring it to the front.";
  a.keywords = "window show focus open";
  a.run = [this, window, shown] {
    if (shown) *shown = true;
    focus_window_ = window;
    focus_tries_ = 4;  // a panel just switched on appears next frame
  };
  actions_.add(std::move(a));
}

void EditorUx::add_settings_entries(std::function<void(const std::string&)> open) {
  for (const SettingsSection& s : settings_sections()) {
    Action a;
    a.id = std::string("Settings/") + s.title;
    a.label = s.title;
    a.path = "Settings";
    a.tip = std::string(s.blurb) + ". Opens Settings (the claim is saved first).";
    a.keywords = std::string("preferences options ") + s.blurb;
    const std::string title = s.title;
    a.run = [open, title] { open(title); };
    actions_.add(std::move(a));
  }
}

void EditorUx::refresh_recent(std::function<void(const RecentItem&)> open) {
  actions_.remove_prefix("Recent/");
  for (const RecentItem& r : prefs().recent) {
    Action a;
    a.id = "Recent/" + r.kind + "/" + r.path;
    a.label = r.label;
    a.path = r.kind == "claim" ? "Recent claim" : "Recent asset";
    a.tip = r.path;
    a.keywords = "recent open " + r.kind;
    const RecentItem copy = r;
    a.run = [open, copy] { open(copy); };
    actions_.add(std::move(a));
  }
}

void EditorUx::harvest(const std::string& invoke) {
  if (!draw_menus_ || !ImGui::GetCurrentContext()) return;
  BeginHarvestWindow();
  actions_.begin_harvest("menu", invoke);
  draw_menus_();
  actions_.end_harvest();
  EndHarvestWindow();
}

void EditorUx::harvest_contexts() {
  if (!draw_contexts_ || !ImGui::GetCurrentContext()) return;
  BeginHarvestWindow();
  actions_.begin_harvest("ctx");
  draw_contexts_();
  actions_.end_harvest();
  EndHarvestWindow();
}

bool EditorUx::run_now(const std::string& id) {
  if (actions_.find_static(id)) return actions_.run(id);
  harvest();
  if (!actions_.run(id)) return false;
  harvest(actions_.take_pending());
  return true;
}

void EditorUx::frame() {
  if (!ImGui::GetCurrentContext()) return;
  actions_.handle_shortcuts();
  if (palette_.is_open()) {
    harvest();
    const std::string id = palette_.draw(actions_.palette_actions());
    if (!id.empty()) actions_.run(id);
  }
  if (actions_.has_pending()) harvest(actions_.take_pending());
  if (!focus_window_.empty()) {
    ImGui::SetWindowFocus(focus_window_.c_str());
    if (--focus_tries_ <= 0) focus_window_.clear();
  }
  walk_.frame([this](const std::string& panel) { run_now("Panel/" + panel); });
  draw_layout_popups();
  draw_error_modal([this](const std::string& id) { return run_now(id); });
}

void EditorUx::begin_frame() {
  if (apply_pending_layout()) layout_status_ += " (applied)";
}

void EditorUx::draw_walkthrough_menu_item() {
  if (MenuItem("Walkthrough")) walk_.start();
}

void EditorUx::reset_layout() {
  if (reset_layout_) reset_layout_();
  layout_status_ = std::string("Reset to the default ") + (three_d_ ? "3D" : "2D") + " layout";
}

void EditorUx::request_save_layout(const std::string& suggested) {
  save_popup_ = true;
  std::snprintf(save_name_, sizeof(save_name_), "%s", suggested.c_str());
}

void EditorUx::draw_window_menu() {
  if (!BeginMenu("Window")) return;
  if (BeginMenu("Layouts")) {
    if (MenuItem("Save current...")) request_save_layout();
    ImGui::Separator();
    if (MenuItem(three_d_ ? "Default 3D" : "Default 2D")) reset_layout();
    const std::vector<std::string> names = list_layouts(three_d_);
    if (!names.empty()) {
      ImGui::Separator();
      for (const std::string& n : names) {
        if (MenuItem(n.c_str())) {
          std::string err;
          if (queue_load_layout(three_d_, n, &err)) layout_status_ = "Loaded layout \"" + n + "\"";
          else show_error(explain(err, ErrorKind::MissingFile, "Loading a saved layout"));
        }
      }
      if (BeginMenu("Delete layout")) {
        for (const std::string& n : names) {
          if (MenuItem(n.c_str())) {
            confirm_delete_ = n;
            confirm_delete_open_ = true;
          }
        }
        EndMenu();
      }
    }
    ImGui::Separator();
    if (MenuItem("Reset layout")) reset_layout();
    EndMenu();
  }
  EndMenu();
}

void EditorUx::draw_layout_popups() {
  const char* save_id = "Save layout###ts_save_layout";
  const char* del_id = "Delete layout?###ts_delete_layout";
  if (save_popup_) {
    ImGui::OpenPopup(save_id);
    save_popup_ = false;
  }
  if (confirm_delete_open_) {
    ImGui::OpenPopup(del_id);
    confirm_delete_open_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal(save_id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Name this panel arrangement. Layouts are yours, kept with your");
    ImGui::TextUnformatted("settings (not in the claim), and listed under Window > Layouts.");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("Name##layout", save_name_, sizeof(save_name_),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string clean = clean_layout_name(save_name_);
    if (!clean.empty() && layout_exists(three_d_, clean)) {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::Warning());
      ImGui::Text("Replaces the saved layout \"%s\".", clean.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::BeginDisabled(clean.empty());
    const bool save = theme::PrimaryButton("Save") || (enter && !clean.empty());
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (theme::SecondaryButton("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    if (save) {
      std::string err;
      if (save_layout(three_d_, clean, &err)) layout_status_ = "Saved layout \"" + clean + "\"";
      else show_error(explain(err, ErrorKind::SaveFailed, "Saving a layout"));
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal(del_id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Delete the saved layout \"%s\"?", confirm_delete_.c_str());
    ImGui::TextDisabled("Your current panels stay as they are. This can't be undone.");
    if (theme::DangerButton("Delete")) {
      std::string err;
      if (delete_layout(three_d_, confirm_delete_, &err)) layout_status_ = "Deleted layout \"" + confirm_delete_ + "\"";
      else show_error(explain(err, ErrorKind::SaveFailed, "Deleting a layout"));
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (theme::SecondaryButton("Keep it") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

}  // namespace ts::tombstone::editor::ux
