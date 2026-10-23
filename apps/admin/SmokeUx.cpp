// --smoke-ux (see SmokeUx.h). Runs both editors headless (null renderer) in
// one ImGui context with docking, like the other editor smokes.
#include "SmokeUx.h"

#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/settings/Settings.h"
#include "editor/store/TradingPostPanel.h"
#include "plugins/Builtins.h"
#include "store/TradingPost.h"
#include "editor/ux/Actions.h"
#include "editor/ux/CommandPalette.h"
#include "editor/ux/EditorUx.h"
#include "editor/ux/EmptyState.h"
#include "editor/ux/Tooltip.h"
#include "editor/ux/UxPrefs.h"
#include "editor/ux/UxStrings.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace ts::tombstone;
namespace ux = ts::tombstone::editor::ux;
namespace s3 = ts::tombstone::scene3d;

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
  // Every menu item explains itself when hovered (the shortcut is added).
  for (const auto& a : menu) {
    if (!a.tip.empty()) continue;
    if (dump()) std::cout << "  [menu-tip-missing] " << t << " " << a.id << " | " << a.label << "\n";
    else return fail(t + ": menu item without a tooltip: " + a.id);
  }
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

// ---- tooltips -----------------------------------------------------------------

// Every labelled widget the toolbar / Inspector drew must have text.
int check_tips(const char* tag, const std::vector<std::string>& scopes, std::size_t min_items) {
  std::vector<std::string> missing;
  std::size_t n = 0;
  for (const ux::SeenItem& it : ux::audit_items()) {
    bool want = false;
    for (const auto& sc : scopes) want |= it.scope == sc;
    if (!want) continue;
    ++n;
    if (!it.has_tip) missing.push_back(it.scope + " [" + it.section + "] " + it.key);
  }
  if (dump())
    for (const auto& m : missing) std::cout << "  [tip-missing] " << m << "\n";
  if (n < min_items) return fail(std::string(tag) + ": only " + std::to_string(n) + " toolbar/Inspector widgets seen");
  if (!missing.empty())
    return fail(std::string(tag) + ": " + std::to_string(missing.size()) + " widgets without a tooltip, first: " +
                missing.front());
  return 0;
}

// A claim with no assets or scripts (the starter files removed).
void empty_claim(const fs::path& dir) {
  std::error_code ec;
  for (const char* sub : {"assets", "scripts", "prefabs"}) {
    fs::remove_all(dir / sub, ec);
    fs::create_directories(dir / sub, ec);
  }
  fs::remove_all(dir / ".tombstone" / "meta", ec);
}

// Each panel, brought to the front, shows its empty state with a way forward.
int check_empty(Ctx& ctx, ux::EditorUx& u, const char* tag, const std::function<void()>& tick,
                const std::function<void()>& quiet,
                const std::vector<std::pair<std::string, std::vector<std::string>>>& want) {
  for (const auto& [panel, ids] : want) {
    quiet();  // the Telegraph is only empty before anything has been logged
    ux::reset_empty_states_seen();
    if (!u.run_now("Panel/" + panel)) return fail(std::string(tag) + ": no palette entry Panel/" + panel);
    bool ok = false;
    for (int f = 0; f < 120 && !ok; ++f) {  // the Vault opens its library on a worker
      ctx.frames(1, [&] { tick(); });
      for (const auto& id : ids) ok |= f >= 4 && ux::empty_states_seen().count(id) > 0;
      if (!ok && f > 4) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (dump()) {
      std::cout << "  [empty] " << tag << " " << panel << ":";
      for (const auto& id : ux::empty_states_seen()) std::cout << " " << id;
      std::cout << "\n";
    }
    if (!ok) return fail(std::string(tag) + ": " + panel + " drew no empty state (wanted " + ids.front() + ")");
  }
  for (const char* id : {"hierarchy2d", "hierarchy3d", "assets", "scripts", "vault", "telegraph", "inspector2d", "inspector3d"}) {
    const ux::EmptyText t = ux::empty_text(id);
    if (std::string(t.title) == "Nothing here yet." || !t.body || std::strlen(t.body) < 40)
      return fail(std::string("empty state text missing for ") + id);
  }
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
  int menu2d = 0, menu3d = 0, tips2d = 0, tips3d = 0;
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

    // Tooltips: one entity wearing every component, then each starter entity.
    for (auto& en : s.workspace().entities()) {
      if (en.tilemap || en.player || en.camera) continue;
      en.collider = ColliderData{};
      en.body = RigidBody2DData{};
      en.joint = Joint2DData{};
      en.audio = AudioSourceData{};
      en.particles = ts::fx::EmitterDesc{};
      en.light = ts::fx::Light2DData{};
      en.animator = AnimatorData{};
      en.follow = FollowData{};
      en.spawn = SpawnPointData{};
      en.script = ScriptData{};
      break;
    }
    // A very tall display: clipped widgets skip ImGui's item hook.
    ImGui::GetIO().DisplaySize = ImVec2(2400, 40000);
    ux::set_audit(true);
    ux::clear_audit();
    std::vector<std::uint64_t> ids;
    for (const auto& en : s.workspace().entities()) ids.push_back(en.id);
    for (std::uint64_t id : ids) {
      s.workspace().select(id);
      ctx.frames(5, [&] { s.on_update(1.0f / 60.0f); });  // audit opens nested headers
    }
    ux::set_audit(false);
    ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
    if (int r = check_tips("2D", {"Toolbar2D", "Inspector2D"}, 60)) return r;
    tips2d = 0;
    for (const auto& it : ux::audit_items()) tips2d += it.scope == "Toolbar2D" || it.scope == "Inspector2D";
    s.on_exit();
  }
  empty_claim(root / "p2d");
  {
    editor::ProjectInfo info;
    info.id = "_ux_2d";
    info.name = "UX 2D";
    info.path = (root / "p2d").string();
    editor::Editor2DScreen s(info);
    s.on_enter();
    ctx.frames(3, [&] { s.on_update(1.0f / 60.0f); });
    // Empty states: nothing selected, then nothing at all.
    s.workspace().clear_selection();
    while (!s.workspace().entities().empty()) {
      s.workspace().select_all();
      if (s.delete_selected() == 0) break;
    }
    if (int r = check_empty(ctx, s.field_guide(), "2D", [&] { s.on_update(1.0f / 60.0f); }, [&] { s.telegraph().clear(); },
                            {{"Hierarchy", {"hierarchy2d"}},
                             {"Inspector", {"inspector2d"}},
                             {"Asset Browser", {"assets", "asset_search"}},
                             {"Scripts", {"scripts"}},
                             {"Vault", {"vault", "vault_search"}},
                             {"Telegraph (Console)", {"telegraph", "telegraph_filter"}}}))
      return r;
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

    // Tooltips: the crate wearing every component, then each starter entity.
    if (const s3::Entity3D* crate = e.scene().find_named("Crate")) {
      s3::Entity3D c = *crate;
      c.collider = s3::Collider3D{};
      c.rigid_body = s3::RigidBody3D{};
      c.joint = s3::Joint3D{};
      c.audio = AudioSourceData{};
      c.particles = ts::fx::EmitterDesc{};
      c.animator = s3::Animator3D{};
      c.agent = s3::Agent3D{};
      c.nav_link = s3::NavLink3D{};
      c.follow = s3::Follow3D{};
      c.grabbable = s3::Grabbable3D{};
      c.script = ScriptData{};
      e.replace_entity(c, "ux smoke");
    }
    // A very tall display: clipped widgets skip ImGui's item hook.
    ImGui::GetIO().DisplaySize = ImVec2(2400, 40000);
    ux::set_audit(true);
    ux::clear_audit();
    std::vector<std::uint64_t> ids;
    for (const auto& en : e.scene().entities) ids.push_back(en.id);
    for (std::uint64_t id : ids) {
      e.select(id);
      ctx.frames(5, [&] { e.on_update(1.0f / 60.0f); });  // audit opens nested headers
    }
    ux::set_audit(false);
    ImGui::GetIO().DisplaySize = ImVec2(1600, 900);
    if (int r = check_tips("3D", {"Toolbar3D", "Inspector3D"}, 60)) return r;
    tips3d = 0;
    for (const auto& it : ux::audit_items()) tips3d += it.scope == "Toolbar3D" || it.scope == "Inspector3D";
    e.on_exit();
  }
  empty_claim(root / "p3d");
  {
    editor::ProjectInfo info;
    info.id = "_ux_3d";
    info.name = "UX 3D";
    info.kind = editor::ProjectKind::ThreeD;
    info.path = (root / "p3d").string();
    editor::Editor3DScreen e(info);
    e.on_enter();
    ctx.frames(3, [&] { e.on_update(1.0f / 60.0f); });
    e.clear_selection();
    while (!e.scene().entities.empty()) {
      for (const auto& en : e.scene().entities) e.select(en.id, true);
      if (e.delete_selected() == 0) break;
    }
    if (int r = check_empty(ctx, e.field_guide(), "3D", [&] { e.on_update(1.0f / 60.0f); }, [&] { e.clear_log(); },
                            {{"Hierarchy", {"hierarchy3d"}},
                             {"Inspector", {"inspector3d"}},
                             {"Asset Browser", {"assets", "asset_search"}},
                             {"Vault", {"vault", "vault_search"}},
                             {"Assets", {"scripts"}},
                             {"Telegraph (Console)", {"telegraph"}}}))
      return r;
    e.on_exit();
  }
  fs::remove_all(root, ec);
  // Trading Post rows explain themselves: what, where it works, what next.
  {
    store::StoreEntry en;
    en.id = "ts.example";
    en.name = "Example Spurs";
    en.version = "1.2.0";
    en.author = "TombStone Team";
    en.description = "Adds spurs.";
    en.types = plugin::kType2D | plugin::kType3D;
    en.source = store::Source::Remote;
    en.origin = "https://example.invalid/index.json";
    const std::string tip = editor::trading_post_entry_tip(en, false);
    for (const char* need : {"Example Spurs", "v1.2.0", "Adds spurs.", "2D / 3D", "Install"})
      if (tip.find(need) == std::string::npos) return fail(std::string("Trading Post tip lacks '") + need + "': " + tip);
    en.installed = true;
    if (editor::trading_post_entry_tip(en, true).find("Hitched") == std::string::npos)
      return fail("Trading Post tip for a hitched plugin doesn't say so");
  }
  std::cout << "[smoke] ux OK (registry: " << menu2d << " 2D / " << menu3d
            << " 3D menu actions, palette fuzzy find + run + Ctrl+Shift+P; tooltips on " << tips2d
            << " 2D / " << tips3d << " 3D toolbar + Inspector widgets + every menu item + Trading Post rows; "
            << "empty states in 6 2D / 6 3D panels)\n";
  return 0;
}
