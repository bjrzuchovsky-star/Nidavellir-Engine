#pragma once

// The Field Guide pieces every editor screen shares: the action registry +
// command palette, panel/settings/recent palette entries, and the hand-offs
// to AppFlow (open Settings at a section, open a recent claim). Each editor
// owns one EditorUx and calls frame() once per frame after its panels.

#include "editor/ux/Actions.h"
#include "editor/ux/CommandPalette.h"
#include "editor/ux/UxPrefs.h"
#include "editor/ux/Walkthrough.h"

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

  // Start of the editor's frame, before any panel: applies a queued layout.
  void begin_frame();
  // Per frame: shortcuts, palette draw + run, pending menu invokes, focus,
  // the walkthrough card and the layout popups.
  void frame();

  // First-run tour (Help > Walkthrough).
  Walkthrough& walkthrough() { return walk_; }
  // Help menu item "Walkthrough" (inside the screen's Help menu).
  void draw_walkthrough_menu_item();
  // The "Window" menu (Layouts submenu) for the screen's menu bar.
  void draw_window_menu();
  // Rebuild this editor's default docking (Default 2D/3D, Reset layout).
  void set_layout_reset(std::function<void()> f) { reset_layout_ = std::move(f); }
  void reset_layout();
  // Window > Layouts > Save current... (opens the name popup).
  void request_save_layout(const std::string& suggested = {});
  // Last layout message ("Saved layout ..."), for the smoke / status line.
  const std::string& layout_status() const { return layout_status_; }
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
  Walkthrough walk_;
  std::function<void()> reset_layout_;
  bool save_popup_ = false;
  char save_name_[64] = {};
  std::string confirm_delete_;
  bool confirm_delete_open_ = false;
  std::string layout_status_;
  void draw_layout_popups();
};

// Hand-offs to AppFlow (read once).
void request_settings_section(const std::string& section);
std::string take_settings_section();
bool settings_section_requested();
void request_open_claim(const std::string& path);
std::string take_open_claim();

}  // namespace ts::tombstone::editor::ux
