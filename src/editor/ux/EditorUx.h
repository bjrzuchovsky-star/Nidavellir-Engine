#pragma once

// The Field Guide pieces every editor screen shares: the action registry +
// command palette, panel/settings/recent palette entries, and the hand-offs
// to AppFlow (open Settings at a section, open a recent claim). Each editor
// owns one EditorUx and calls frame() once per frame after its panels.

#include "editor/ux/Actions.h"
#include "editor/ux/CommandPalette.h"
#include "editor/ux/UxPrefs.h"

#include <functional>
#include <string>
#include <vector>

namespace ts::tombstone::editor::ux {

class EditorUx {
 public:
  explicit EditorUx(bool three_d);

  ActionRegistry& actions() { return actions_; }
  CommandPalette& palette() { return palette_; }
  bool three_d() const { return three_d_; }

  // The screen's menu bar contents (no BeginMainMenuBar): drawn offscreen to
  // harvest / invoke menu items.
  void set_menu_drawer(std::function<void()> f) { draw_menus_ = std::move(f); }
  // Context menus for --smoke-ux: draws every context menu body under its
  // ux::BeginContextScope.
  void set_context_drawer(std::function<void()> f) { draw_contexts_ = std::move(f); }

  // A dockable panel: palette "Panel > Inspector" shows and focuses it.
  void add_panel(const std::string& label, const std::string& window, bool* shown);
  // Palette entries for each Settings section; open(section) leaves for
  // Settings (the screen decides how).
  void add_settings_entries(std::function<void(const std::string&)> open);
  // Palette entries for recent claims / assets (from ux::prefs()).
  void refresh_recent(std::function<void(const RecentItem&)> open);

  // Per frame: shortcuts, palette draw + run, pending menu invokes, focus.
  void frame();
  // Harvest the menus now (smoke + palette); invoke = id to run.
  void harvest(const std::string& invoke = {});
  void harvest_contexts();
  // Run an action by id, menu items included (harvests immediately).
  bool run_now(const std::string& id);

 private:
  bool three_d_ = false;
  ActionRegistry actions_;
  CommandPalette palette_;
  std::function<void()> draw_menus_;
  std::function<void()> draw_contexts_;
  std::string focus_window_;
  int focus_tries_ = 0;
};

// Hand-offs to AppFlow (read once).
void request_settings_section(const std::string& section);
std::string take_settings_section();
bool settings_section_requested();
void request_open_claim(const std::string& path);
std::string take_open_claim();

}  // namespace ts::tombstone::editor::ux
