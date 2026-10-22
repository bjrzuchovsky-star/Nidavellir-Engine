#pragma once

// The action registry behind menus, shortcuts and the command palette.
//
// Menus are written with ux::BeginMenu / ux::MenuItem instead of ImGui's.
// Drawn normally they behave exactly like ImGui's (plus a Field Guide tip).
// Drawn in *harvest* mode (offscreen, no popups) every item reports itself
// to the registry with its path, shortcut, enabled and checked state, so the
// palette always lists exactly what the menus hold. Running a harvested item
// re-harvests with that id armed: its own `if (ux::MenuItem(...))` body runs,
// so a menu item, its shortcut label and its palette entry cannot drift.
//
// Actions that live outside the menus (focus a panel, open a Settings
// section, a recent claim, the palette itself) are added as static actions
// with a run() callback and an optional shortcut chord.
//
// Context menus use the same wrappers under a "ctx.*" scope; --smoke-ux
// harvests them to check every expected item is there.

#include <functional>
#include <map>
#include <string>
#include <vector>

struct ImGuiContext;

namespace ts::tombstone::editor::ux {

struct Action {
  std::string id;        // "Edit/Duplicate", "Panel/Inspector"
  std::string label;     // "Duplicate"
  std::string path;      // "Edit" ("Object/Align" for submenus)
  std::string shortcut;  // display text, "Ctrl+D"
  std::string tip;
  std::string scope = "menu";  // "menu" | "palette" | "ctx.object" ...
  bool enabled = true;
  bool checkable = false;
  bool checked = false;
  std::function<bool()> is_enabled;  // static actions
  std::function<void()> run;         // static actions
  std::string keywords;              // extra fuzzy search words
};

class ActionRegistry {
 public:
  // ---- static actions -----------------------------------------------------
  void add(Action a);  // replaces an action with the same id
  void remove_prefix(const std::string& id_prefix);
  const Action* find_static(const std::string& id) const;

  // ---- harvest ------------------------------------------------------------
  // Between begin/end, ux::BeginMenu / ux::MenuItem record instead of draw.
  // invoke_id arms one item: its MenuItem returns true (and flips a bool*).
  void begin_harvest(const std::string& scope, const std::string& invoke_id = {});
  void end_harvest();
  const std::vector<Action>& harvested(const std::string& scope) const;
  // Widgets drawn by menu code outside ux::MenuItem during the last harvest
  // of a scope (raw ImGui::MenuItem etc.): the audit wants this empty.
  const std::vector<std::string>& unregistered(const std::string& scope) const;

  // Everything the palette lists: harvested menu items + static actions.
  std::vector<Action> palette_actions() const;

  // Run by id. Static actions run now (when enabled); menu items queue and
  // run on the next harvest (call take_pending + harvest from the screen).
  bool run(const std::string& id);
  bool has_pending() const { return !pending_.empty(); }
  std::string take_pending();
  int run_count(const std::string& id) const;
  void note_ran(const std::string& id) { ++runs_[id]; }

  // Static actions with a chord ("Ctrl+Shift+P") fire from the keyboard.
  void handle_shortcuts();

  bool harvesting() const { return harvesting_; }

 private:
  friend struct HarvestAccess;
  std::vector<Action> statics_;
  std::map<std::string, std::vector<Action>> harvested_;
  std::map<std::string, std::vector<std::string>> unregistered_;
  std::map<std::string, int> runs_;
  std::string pending_;
  bool harvesting_ = false;
};

// ---- menu wrappers (ImGui-compatible) ---------------------------------------
bool BeginMenu(const char* label, bool enabled = true);
void EndMenu();
bool MenuItem(const char* label, const char* shortcut = nullptr, bool selected = false,
              bool enabled = true);
bool MenuItem(const char* label, const char* shortcut, bool* p_selected, bool enabled = true);
// Context menu roots: name the scope items are filed under while harvesting.
void BeginContextScope(const char* scope);
void EndContextScope();

// Option widgets inside a menu (checkboxes that tune the items below, not
// actions themselves): bracket them so the coverage audit skips them.
void BeginMenuOptions();
void EndMenuOptions();

// Offscreen host window for a harvest pass (Begin/End around it).
void BeginHarvestWindow();
void EndHarvestWindow();

// Called by the item hook (Tooltip.cpp): flags raw widgets in a harvest.
void menu_audit_item(ImGuiContext* ctx, unsigned int id, const char* label, int flags);

// Parse "Ctrl+Shift+P" and test it this frame (no repeat).
bool ChordPressed(const std::string& chord);

}  // namespace ts::tombstone::editor::ux
