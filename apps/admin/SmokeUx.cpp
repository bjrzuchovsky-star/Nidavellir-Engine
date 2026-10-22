// --smoke-ux (see SmokeUx.h). Runs both editors headless (null renderer) in
// one ImGui context with docking, like the other editor smokes.
#include "SmokeUx.h"

#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/settings/Settings.h"
#include "editor/ux/Actions.h"
#include "editor/ux/CommandPalette.h"
#include "editor/ux/EditorUx.h"
#include "editor/ux/Tooltip.h"
#include "editor/ux/UxPrefs.h"
#include "editor/ux/UxStrings.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ts::tombstone;
namespace ux = ts::tombstone::editor::ux;

namespace {

int fail(const std::string& m) {
  std::cerr << "[smoke] ux FAIL: " << m << "\n";
  return 1;
}

bool dump() { return std::getenv("TS_UX_DUMP") != nullptr; }

// One ImGui context for the whole smoke (screens keep docking state).
struct Ctx {
  ImGuiContext* prev = nullptr;
  ImGuiContext* c = nullptr;
  Ctx() {
    prev = ImGui::GetCurrentContext();
    c = ImGui::CreateContext();
    ImGui::SetCurrentContext(c);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600, 900);
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    unsigned char* px = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    ux::install_hook();
  }
  ~Ctx() {
    ImGui::DestroyContext(c);
    ImGui::SetCurrentContext(prev);
  }
  template <class F>
  void frames(int n, F&& body) {
    for (int i = 0; i < n; ++i) {
      ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
      ImGui::NewFrame();
      body();
      ImGui::Render();
    }
  }
};

// Keep the rider's real settings.json untouched.
struct SettingsBackup {
  std::string path = editor::SettingsStore::default_settings_path();
  bool had = false;
  std::string text;
  SettingsBackup() {
    std::ifstream in(path, std::ios::binary);
    if (in) {
      had = true;
      std::stringstream ss;
      ss << in.rdbuf();
      text = ss.str();
    }
  }
  ~SettingsBackup() {
    std::error_code ec;
    if (had) {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out << text;
    } else {
      fs::remove(path, ec);
    }
  }
};

bool has_id(const std::vector<ux::Action>& v, const std::string& id) {
  for (const auto& a : v)
    if (a.id == id) return true;
  return false;
}

const ux::Action* find(const std::vector<ux::Action>& v, const std::string& id) {
  for (const auto& a : v)
    if (a.id == id) return &a;
  return nullptr;
}

void dump_actions(const char* what, const std::vector<ux::Action>& v) {
  if (!dump()) return;
  for (const auto& a : v)
    std::cout << "  [" << what << "] " << a.id << " | " << a.label << " | " << a.shortcut
              << (a.enabled ? "" : " (disabled)") << (a.tip.empty() ? " (no tip)" : "") << "\n";
}

// ---- registry + palette -----------------------------------------------------

int check_registry(Ctx& ctx, ux::EditorUx& u, const char* tag,
                   const std::vector<std::string>& must_have, const std::string& run_toggle,
                   const std::function<bool()>& toggle_state) {
  const std::string t = tag;
  ctx.frames(1, [&] { u.harvest(); });
  const auto& menu = u.actions().harvested("menu");
  dump_actions(tag, menu);
  if (menu.size() < 40) return fail(t + ": only " + std::to_string(menu.size()) + " menu actions harvested");
  const auto& raw = u.actions().unregistered("menu");
  if (!raw.empty()) {
    std::string list;
    for (const auto& r : raw) list += " '" + r + "'";
    return fail(t + ": menu widgets outside the registry:" + list);
  }
  for (const auto& id : must_have)
    if (!has_id(menu, id)) return fail(t + ": menu action missing: " + id);
  for (const auto& a : menu)
    if (a.label.empty() || a.id.empty()) return fail(t + ": action without label/id under " + a.path);

  // Palette lists every menu item + panels + settings sections.
  const std::vector<ux::Action> all = u.actions().palette_actions();
  for (const auto& a : menu)
    if (!has_id(all, a.id)) return fail(t + ": palette missing " + a.id);
  for (const char* sec : {"Settings/Tools", "Settings/Field Guide"})
    if (!has_id(all, sec)) return fail(t + std::string(": palette missing ") + sec);
  if (!has_id(all, "Panel/Inspector") || !has_id(all, "Panel/Hierarchy"))
    return fail(t + ": palette panels");
  if (!has_id(all, "Palette/Command Palette")) return fail(t + ": palette's own entry");
  {
    const ux::Action* p = find(all, "Palette/Command Palette");
    if (!p || p->shortcut != "Ctrl+Shift+P") return fail(t + ": palette chord label");
  }

  // Fuzzy search: a sloppy query still finds the item, best first.
  for (const auto& id : must_have) {
    const ux::Action* a = find(all, id);
    std::string q;
    for (char ch : a->label)  // every other letter: "Dpiae" for Duplicate
      if (q.size() < 5 && ((&ch - a->label.data()) % 2 == 0) && ch != ' ') q += ch;
    const auto ranked = ux::palette_rank(a->label, all);
    if (ranked.empty()) return fail(t + ": palette finds nothing for " + a->label);
    // Disabled entries sort after enabled ones, so rank among the same kind.
    bool top = false;
    int seen = 0;
    for (std::size_t i = 0; i < ranked.size() && seen < 3; ++i) {
      if (all[ranked[i]].enabled != a->enabled) continue;
      top |= all[ranked[i]].id == id;
      ++seen;
    }
    if (!top) return fail(t + ": '" + a->label + "' not in the top 3");
    if (ux::palette_rank(q, all).empty()) return fail(t + ": fuzzy '" + q + "' finds nothing");
  }
  if (!ux::palette_rank("zzqqxx", all).empty()) return fail(t + ": nonsense query matched");

  // Running a menu toggle through the registry flips the real state.
  const bool before = toggle_state();
  if (!u.run_now(run_toggle)) return fail(t + ": run " + run_toggle);
  ctx.frames(1, [&] { u.frame(); });
  if (toggle_state() == before) return fail(t + ": " + run_toggle + " did not toggle");
  if (u.actions().run_count(run_toggle) < 1) return fail(t + ": run count");
  u.run_now(run_toggle);
  if (toggle_state() != before) return fail(t + ": toggle back");

  // Disabled items refuse to run.
  for (const auto& a : menu) {
    if (a.enabled) continue;
    if (u.actions().run(a.id)) return fail(t + ": disabled " + a.id + " ran");
    break;
  }

  // Ctrl+Shift+P opens the palette, Escape closes it.
  ImGuiIO& io = ImGui::GetIO();
  ctx.frames(1, [&] {
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddKeyEvent(ImGuiKey_LeftCtrl, true);
    io.AddKeyEvent(ImGuiKey_LeftShift, true);
    io.AddKeyEvent(ImGuiKey_P, true);
  });
  ctx.frames(1, [&] { u.frame(); });
  ctx.frames(1, [&] {
    io.AddKeyEvent(ImGuiKey_P, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiKey_LeftCtrl, false);
    io.AddKeyEvent(ImGuiKey_LeftShift, false);
    u.frame();
  });
  if (!u.palette().is_open()) return fail(t + ": Ctrl+Shift+P did not open the palette");
  // Type a query and press Enter: the top entry runs.
  u.palette().set_query("Panel Inspector");
  ctx.frames(1, [&] { u.frame(); });
  ctx.frames(1, [&] {
    io.AddKeyEvent(ImGuiKey_Enter, true);
    u.frame();
  });
  ctx.frames(1, [&] {
    io.AddKeyEvent(ImGuiKey_Enter, false);
    u.frame();
  });
  if (u.actions().run_count("Panel/Inspector") < 1) return fail(t + ": palette Enter did not run Panel/Inspector");
  if (u.palette().is_open()) return fail(t + ": palette stays open after running");
  return 0;
}

}  // namespace

int run_ux_smoke() {
  SettingsBackup keep;
  const fs::path root = fs::absolute("./TombStoneProjects/_ux_smoke");
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "p2d", ec);
  fs::create_directories(root / "p3d", ec);
  ux::prefs() = ux::Prefs{};
  ux::prefs().walkthrough_2d_done = ux::prefs().walkthrough_3d_done = true;  // tours tested on their own
  Ctx ctx;
  int menu2d = 0, menu3d = 0;
  {
    editor::ProjectInfo info;
    info.id = "_ux_2d";
    info.name = "UX 2D";
    info.path = (root / "p2d").string();
    editor::Editor2DScreen s(info);
    s.on_enter();
    ctx.frames(3, [&] { s.on_update(1.0f / 60.0f); });
    if (int r = check_registry(ctx, s.field_guide(), "2D",
                               {"File/Save Scene", "Edit/Duplicate", "Edit/EditUndo"}, "View/Hierarchy",
                               [&] { return s.hierarchy_shown(); }))
      return r;
    menu2d = static_cast<int>(s.field_guide().actions().harvested("menu").size());
    s.on_exit();
  }
  {
    editor::ProjectInfo info;
    info.id = "_ux_3d";
    info.name = "UX 3D";
    info.kind = editor::ProjectKind::ThreeD;
    info.path = (root / "p3d").string();
    editor::Editor3DScreen e(info);
    e.on_enter();
    ctx.frames(3, [&] { e.on_update(1.0f / 60.0f); });
    if (int r = check_registry(ctx, e.field_guide(), "3D",
                               {"Claim/Save scene", "Edit/Duplicate", "Edit/Undo", "Create/Cube"},
                               "View/Grid", [&] { return e.scene().show_grid; }))
      return r;
    menu3d = static_cast<int>(e.field_guide().actions().harvested("menu").size());
    e.on_exit();
  }
  fs::remove_all(root, ec);
  std::cout << "[smoke] ux OK (registry: " << menu2d << " 2D / " << menu3d
            << " 3D menu actions, palette fuzzy find + run + Ctrl+Shift+P)\n";
  return 0;
}
