#include "editor/ux/EditorUx.h"
#include "editor/ux/FriendlyError.h"

#include "editor/ux/UxPrefs.h"
#include "editor/ux/UxStrings.h"

#include <imgui.h>

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

EditorUx::EditorUx(bool three_d) : three_d_(three_d) {
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
  draw_error_modal([this](const std::string& id) { return run_now(id); });
}

}  // namespace ts::tombstone::editor::ux
