// Plugins in the 2D editor: load on enter, the Plugins (Outfitter) panel,
// and drawing what loaded plugins contribute (panels, menu entries,
// toolbar buttons). See editor/plugins/EditorPlugins.h.

#include "editor/screens/Editor2DScreen.h"

#include "editor/ui/Theme.h"

#include <imgui.h>

#include <set>

namespace ts {
namespace tombstone {
namespace editor {

void Editor2DScreen::open_plugins() {
  plugins_applying_ = true;
  plugins_.open(&project_, "editor2d", this,
                [this](runtime::LogLevel level, const std::string& text) {
                  telegraph_.add(level, "plugins", text);
                  if (level == runtime::LogLevel::Error) focus_console_ = true;
                });
  plugins_applying_ = false;
}

bool Editor2DScreen::apply_plugin_changes_if_idle() {
  if (plugins_applying_ || !plugins_.is_open() || !plugins_.pending()) {
    return false;
  }
  if (is_playing() || posse_running()) return false;
  plugins_applying_ = true;
  const std::size_t n = plugins_.apply();
  plugins_applying_ = false;
  note("Plugins re-hitched: " + std::to_string(n) + " riding with this claim.");
  return true;
}

bool Editor2DScreen::set_plugin_enabled(const std::string& id, bool on) {
  std::string err;
  if (!plugins_.set_enabled(id, on, &err)) {
    if (!err.empty()) telegraph_.add(runtime::LogLevel::Error, "plugins", err);
    return false;
  }
  if (plugins_.pending() && (is_playing() || posse_running())) {
    note("Saved. The change takes hold once this ride's over.");
  }
  apply_plugin_changes_if_idle();
  return true;
}

void Editor2DScreen::draw_plugins_window() {
  if (!show_plugins_ || !plugins_.is_open()) return;
  ImGui::SetNextWindowSize(ImVec2(340, 420), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Plugins", &show_plugins_)) {
    const bool riding = is_playing() || posse_running();
    const PluginsPanelResult r =
        draw_plugins_panel(plugins_, plugins_panel_, riding);
    if (r.changed) {
      if (riding && plugins_.pending()) {
        note("Saved. The change takes hold once this ride's over.");
      }
      apply_plugin_changes_if_idle();
    }
    if (r.rescan_requested && !riding) {
      plugins_applying_ = true;
      const std::size_t n = plugins_.rescan();
      plugins_applying_ = false;
      note("Wagon rescanned: " + std::to_string(plugins_.catalog().entries().size()) +
           " plugins on hand, " + std::to_string(n) + " hitched.");
    }
  }
  ImGui::End();
}

void Editor2DScreen::draw_plugin_view_items() {
  if (!plugins_.is_open()) return;
  for (const auto& p : plugins_.manager().editor_panels()) {
    bool* open = p.item.open ? p.item.open : &plugin_panel_open_[p.item.id];
    ImGui::MenuItem(p.item.menu_label.c_str(), nullptr, open);
  }
}

void Editor2DScreen::draw_plugin_panels() {
  if (!plugins_.is_open()) return;
  // Copy: a draw callback may change plugin state (never unloads, though:
  // reloads wait for idle and happen outside the panel loop).
  for (const auto& p : plugins_.manager().editor_panels()) {
    bool* open = p.item.open ? p.item.open : &plugin_panel_open_[p.item.id];
    if (!*open) continue;
    if (p.item.before_begin) p.item.before_begin();
    const std::string title = p.item.title + "###plugin_" + p.item.id;
    // The posse's Network panel keeps its old window id for saved layouts.
    const char* window = p.item.title == "Network" ? "Network" : title.c_str();
    if (ImGui::Begin(window, open)) {
      p.item.draw();
    }
    ImGui::End();
  }
}

void Editor2DScreen::draw_plugin_menu_items(const std::string& menu) {
  if (!plugins_.is_open()) return;
  for (const auto& m : plugins_.manager().menu_items()) {
    const plugin::EditorMenuItem& it = m.item;
    if (it.menu != menu) continue;
    ImGui::PushID(&it);
    if (it.draw) {
      it.draw();
    } else if (it.label == "-") {
      ImGui::Separator();
    } else {
      const bool enabled = it.enabled ? it.enabled() : true;
      const bool checked = it.checked ? it.checked() : false;
      if (ImGui::MenuItem(it.label.c_str(),
                          it.shortcut.empty() ? nullptr : it.shortcut.c_str(),
                          checked, enabled) &&
          it.action) {
        it.action();
      }
    }
    ImGui::PopID();
  }
}

void Editor2DScreen::draw_plugin_top_menus() {
  if (!plugins_.is_open()) return;
  static const std::set<std::string> kBuiltIn = {"File", "Edit", "View", "Play",
                                                 "Help", "Toolbar"};
  std::vector<std::string> menus;
  for (const auto& m : plugins_.manager().menu_items()) {
    if (kBuiltIn.count(m.item.menu)) continue;
    bool seen = false;
    for (const std::string& s : menus) seen = seen || s == m.item.menu;
    if (!seen) menus.push_back(m.item.menu);
  }
  for (const std::string& menu : menus) {
    if (ImGui::BeginMenu(menu.c_str())) {
      draw_plugin_menu_items(menu);
      ImGui::EndMenu();
    }
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
