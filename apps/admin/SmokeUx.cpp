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
#include "editor/ux/FriendlyError.h"
#include "editor/ux/Layouts.h"
#include "editor/ux/Walkthrough.h"
#include "editor/vault/VaultPanel.h"
#include "editor/workspace/SceneIO.h"
#include "editor/ux/Tooltip.h"
#include "editor/ux/UxPrefs.h"
#include "editor/ux/UxStrings.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
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

// Readable errors: every case the editor can hit gets a title and plain
// what / why / what-to-do text, keeps the raw message for Copy details, and
// the modal, Telegraph tag, Vault and Trading Post cards all use it. The
// messages are the ones the engine really produces.
std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

int check_friendly(const ux::FriendlyError& f, const std::string& raw, const std::string& label) {
  if (f.title.empty() || f.what.size() < 20 || f.why.size() < 20 || f.fix.size() < 20)
    return fail(label + ": thin explanation (" + f.title + ")");
  if (f.details.find(raw) == std::string::npos) return fail(label + ": details lose the raw message");
  const std::string c = f.copy_text();
  if (c.find(f.title) == std::string::npos || c.find("Details:") == std::string::npos ||
      c.find(raw) == std::string::npos || c.find("What to do:") == std::string::npos)
    return fail(label + ": Copy details text is incomplete");
  return 0;
}

int check_errors(Ctx& ctx, const fs::path& root, int* cases) {
  struct Case {
    const char* raw;
    ux::ErrorKind hint;
    ux::ErrorKind want;
  };
  using K = ux::ErrorKind;
  const Case cases_[] = {
      {"No such file: levels/intro.json", K::Unknown, K::MissingFile},
      {"Script not found -- Gate (scripts/gate.lua)", K::Unknown, K::MissingFile},
      {"cannot open or decode assets/textures/rock.png", K::Unknown, K::MissingTexture},
      {"could not load assets/models/wagon.glb: buffer 0 not found", K::Unknown, K::MissingModel},
      {"Import failed: assets/broken.png: not a PNG or JPEG the importer knows", K::ImportFailed, K::ImportFailed},
      {"scripts/gate.lua:12: 'end' expected (to close 'function' at line 3) near <eof>", K::ScriptRuntime,
       K::ScriptCompile},
      {"scripts/gate.lua:20: attempt to index a nil value (field 'door') -- Gate is stopped; the rest of the world "
       "rides on",
       K::ScriptRuntime, K::ScriptRuntime},
      {"'ts.weather' is switched on for this project but isn't in the wagon (missing plugin library?); riding on "
       "without it",
       K::Unknown, K::PluginMissing},
      {"Rider 3D needs ts.jolt 0.2.0+, which isn't installed", K::Unknown, K::PluginMissing},
      {"ts_game runtime not found (build ts_game first)", K::Unknown, K::ExportFailed},
      {"ISCC not found (install Inno Setup 6: tools/installer/install_inno.ps1, or set TS_ISCC)", K::Unknown,
       K::ExportFailed},
      {"could not connect to vault.example:7777 (connection refused)", K::VaultConnection, K::VaultConnection},
      {"crate.blend: Blender was not found. Set Settings > Tools > Blender executable (or the TS_BLENDER "
       "environment variable, or this asset's Blender path) to your blender.exe, install Blender (blender.org, "
       "Steam, Microsoft Store), or export the file to glTF / FBX. Looked in: PATH",
       K::ImportFailed, K::BlenderNotFound},
      {"could not reach the release: timed out", K::UpdateCheck, K::UpdateCheck},
      {"download failed: HTTP 503", K::UpdateCheck, K::UpdateCheck},
      {"Could not write scene.json.tmp: Permission denied", K::SaveFailed, K::SaveFailed},
      {"scene.json is a 2D scene (no \"dimension\": \"3d\"); open it in a 2D claim", K::MissingFile, K::SceneVersion},
  };
  std::set<K> covered;
  for (const Case& c : cases_) {
    const ux::FriendlyError f = ux::explain(c.raw, c.hint);
    if (f.kind != c.want)
      return fail(std::string("'") + c.raw + "' read as " + ux::to_string(f.kind) + ", wanted " + ux::to_string(c.want));
    if (int r = check_friendly(f, c.raw, ux::to_string(c.want))) return r;
    covered.insert(f.kind);
    if (dump()) std::cout << "[error] " << ux::to_string(f.kind) << ": " << f.title << "\n";
  }
  // Telegraph tags problem lines, leaves info lines alone.
  const std::string tl = ux::telegraph_label(cases_[6].raw, true, ux::telegraph_hint("script"));
  if (tl.find("script stopped") == std::string::npos) return fail("Telegraph line has no plain-words tag: " + tl);
  if (ux::telegraph_label("trigger: Rider rode into Gate", false, K::Unknown) != "trigger: Rider rode into Gate")
    return fail("Telegraph tagged an info line");
  // Panel cards.
  if (editor::friendly_vault_error("could not connect to vault.example:7777", true).kind != K::VaultConnection ||
      editor::friendly_vault_error("x", true).fix_label != "Vault settings")
    return fail("Vault error card isn't the friendly one");
  if (editor::friendly_trading_post_error("Example Spurs: download failed: could not resolve example.invalid").title !=
      "Couldn't reach the Trading Post")
    return fail("Trading Post error card isn't the friendly one");

  // Scene version, for real: a scene.json from the future opens with the
  // readable error and is never saved over, in both editors.
  const std::string future2d = "{\n  \"version\": 999,\n  \"entities\": []\n}\n";
  {
    const fs::path dir = root / "future2d";
    fs::create_directories(dir);
    const fs::path sp = editor::scene_io::scene_path_for_project(dir.string());
    std::ofstream(sp, std::ios::binary) << future2d;
    ux::clear_errors();
    editor::ProjectInfo info;
    info.id = "_ux_future2d";
    info.name = "Future 2D";
    info.path = dir.string();
    editor::Editor2DScreen s(info);
    s.on_enter();
    const ux::FriendlyError* f = ux::current_error();
    if (!f || f->kind != K::SceneVersion) return fail("2D: newer scene.json opened without the scene-version error");
    if (int r = check_friendly(*f, "is newer than this build reads", "2D scene version")) return r;
    // The modal is drawn, with its buttons.
    ux::set_audit(true);
    ux::clear_audit();
    const std::string title = f->title + "###ts_friendly_error";
    ctx.frames(3, [&] { s.on_update(1.0f / 60.0f); });
    ImGuiWindow* w = ImGui::FindWindowByName(title.c_str());
    if (!w || !w->Active) {
      ctx.frames(1, [&] {
        s.on_update(1.0f / 60.0f);
        w = ImGui::FindWindowByName(title.c_str());
      });
    }
    ux::set_audit(false);
    if (!ImGui::FindWindowByName(title.c_str())) return fail("2D: the error modal didn't open");
    std::string err;
    if (s.save_scene(&err)) return fail("2D: saved over a scene from a newer TombStone");
    s.on_exit();
    if (read_all(sp) != future2d) return fail("2D: the newer scene.json was changed on disk");
    covered.insert(K::SceneVersion);
    ux::clear_errors();
  }
  {
    const fs::path dir = root / "future3d";
    fs::create_directories(dir);
    const std::string future3d = "{\n  \"dimension\": \"3d\",\n  \"version\": 999,\n  \"entities\": []\n}\n";
    const fs::path sp = s3::scene_path(dir.string());
    fs::create_directories(fs::path(sp).parent_path());
    std::ofstream(sp, std::ios::binary) << future3d;
    editor::ProjectInfo info;
    info.id = "_ux_future3d";
    info.name = "Future 3D";
    info.kind = editor::ProjectKind::ThreeD;
    info.path = dir.string();
    editor::Editor3DScreen e(info);
    e.on_enter();
    const ux::FriendlyError* f = ux::current_error();
    if (!f || f->kind != K::SceneVersion)
      return fail("3D: newer scene.json opened without the scene-version error" +
                  (f ? std::string(" (got ") + ux::to_string(f->kind) + ": " + f->details + ")" : std::string()));
    ctx.frames(3, [&] { e.on_update(1.0f / 60.0f); });
    std::string err;
    if (e.save_scene(&err)) return fail("3D: saved over a scene from a newer TombStone");
    e.on_exit();
    if (read_all(sp) != future3d) return fail("3D: the newer scene.json was changed on disk");
    ux::clear_errors();
  }
  for (K k : ux::all_error_kinds())
    if (!covered.count(k)) return fail(std::string("no smoke case for ") + ux::to_string(k));
  *cases = static_cast<int>(sizeof(cases_) / sizeof(cases_[0])) + 2;
  return 0;
}

// Reads settings.json back the way the next launch would.
ux::Prefs prefs_on_disk() {
  const ux::Prefs live = ux::prefs();
  ux::import_from(editor::SettingsStore::load());
  ux::Prefs disk = ux::prefs();
  ux::prefs() = live;
  return disk;
}

std::string find_action(const std::vector<ux::Action>& v, const std::string& tail) {
  for (const ux::Action& a : v)
    if (a.id.size() >= tail.size() && a.id.compare(a.id.size() - tail.size(), tail.size(), tail) == 0) return a.id;
  return {};
}

// First-run tour: auto start, every step points at a live panel, skip vs
// "Don't show again", finish, Help > Walkthrough re-run, nothing on the
// second visit. Then layouts: save / load / reset round trip.
template <class Screen>
int check_tour_and_layouts(Ctx& ctx, Screen& s, bool three_d, int* steps_out) {
  const std::string t = three_d ? "3D" : "2D";
  ux::EditorUx& u = s.field_guide();
  ux::Walkthrough& w = u.walkthrough();
  auto tick = [&](int n) { ctx.frames(n, [&] { s.on_update(1.0f / 60.0f); }); };
  auto done = [&](const ux::Prefs& p) { return three_d ? p.walkthrough_3d_done : p.walkthrough_2d_done; };

  tick(8);
  if (!w.active() || !w.auto_started()) return fail(t + ": walkthrough didn't start on the first visit");
  if (!ImGui::FindWindowByName("###ts_walkthrough")) return fail(t + ": no walkthrough card drawn");
  const auto& steps = ux::walkthrough_steps(three_d);
  if (w.step_count() < 5 || w.step_count() > 8) return fail(t + ": walkthrough wants 5-8 steps");
  bool saw_gizmo = false, saw_import = false, saw_play = false, saw_post = false, saw_vault = false, saw_add = false;
  for (int i = 0; i < w.step_count(); ++i) {
    if (w.step() != i) return fail(t + ": walkthrough step order");
    const ux::WalkStep& st = steps[static_cast<std::size_t>(i)];
    tick(4);
    if (*st.window) {
      ImGuiWindow* win = ImGui::FindWindowByName(st.window);
      if (!win || !win->WasActive) return fail(t + ": walkthrough step '" + st.title + "' points at a panel that isn't up: " + st.window);
    }
    const std::string body = std::string(st.title) + " " + st.body;
    saw_gizmo |= body.find("G") != std::string::npos && body.find("R") != std::string::npos && body.find("S") != std::string::npos &&
                 std::string(st.window).find("Viewport") != std::string::npos;
    saw_add |= body.find("Add Component") != std::string::npos;
    saw_import |= body.find("mport") != std::string::npos;
    saw_play |= body.find("Play") != std::string::npos;
    saw_post |= body.find("Trading Post") != std::string::npos;
    saw_vault |= body.find("Vault") != std::string::npos;
    if (i + 1 < w.step_count()) w.next();
  }
  if (!(saw_gizmo && saw_add && saw_import && saw_play && saw_post && saw_vault))
    return fail(t + ": walkthrough misses a stop (viewport G/R/S, Add Component, import, Play, Trading Post, Vault)");
  w.back();
  if (w.step() != w.step_count() - 2) return fail(t + ": walkthrough Back");
  // Skip without "Don't show again": comes back next launch.
  w.dont_show_again = false;
  w.stop(w.dont_show_again);
  tick(2);
  if (w.active() || ImGui::FindWindowByName("###ts_walkthrough")->WasActive) return fail(t + ": Skip left the card up");
  if (done(ux::prefs()) || done(prefs_on_disk())) return fail(t + ": Skip without 'Don't show again' still marked the tour seen");
  // Help > Walkthrough runs it again.
  u.harvest();
  const std::string help = find_action(u.actions().harvested("menu"), "/Walkthrough");
  if (help.empty() || !u.run_now(help)) return fail(t + ": no Help > Walkthrough");
  if (!w.active() || w.step() != 0) return fail(t + ": Help > Walkthrough didn't restart the tour");
  // Skip with "Don't show again" ticked (the default): saved to settings.json.
  if (!w.dont_show_again) return fail(t + ": 'Don't show again' should start ticked");
  w.stop(w.dont_show_again);
  if (!done(prefs_on_disk())) return fail(t + ": 'Don't show again' didn't reach settings.json");
  // Finishing also counts; run it from Help and Next all the way.
  (three_d ? ux::prefs().walkthrough_3d_done : ux::prefs().walkthrough_2d_done) = false;
  u.run_now(help);
  for (int i = 0; i < 20 && w.active(); ++i) w.next();
  if (w.active() || !done(prefs_on_disk())) return fail(t + ": Finish didn't mark the tour seen");
  *steps_out = w.step_count();

  // ---- layouts ----
  const char* panel = three_d ? "Inspector###Inspector3D" : "Inspector";
  u.run_now("Panel/Inspector");
  tick(4);
  ImGuiWindow* insp = ImGui::FindWindowByName(panel);
  if (!insp || insp->DockId == 0) return fail(t + ": Inspector isn't docked by default");
  const ImGuiID docked = insp->DockId;
  const std::string a = "_ux smoke docked", b = "_ux smoke floating";
  std::string err;
  if (!ux::save_layout(three_d, a, &err)) return fail(t + ": save layout: " + err);
  // Float the Inspector, save that too.
  ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), insp);
  tick(3);
  if (insp->DockId != 0) return fail(t + ": couldn't float the Inspector for the layout test");
  // Save through the menu + popup path (Window > Layouts > Save current...).
  u.harvest();
  const std::vector<ux::Action> menu = u.actions().harvested("menu");
  const std::string save_id = find_action(menu, "Layouts/Save current...");
  const std::string reset_id = find_action(menu, "Layouts/Reset layout");
  const std::string def_id = find_action(menu, three_d ? "Layouts/Default 3D" : "Layouts/Default 2D");
  if (save_id.empty() || reset_id.empty() || def_id.empty()) return fail(t + ": Window > Layouts menu incomplete");
  if (!ux::save_layout(three_d, b, &err)) return fail(t + ": save layout: " + err);
  const std::vector<std::string> names = ux::list_layouts(three_d);
  if (std::find(names.begin(), names.end(), a) == names.end() || std::find(names.begin(), names.end(), b) == names.end())
    return fail(t + ": saved layouts not listed");
  {
    std::ifstream in(fs::path(ux::layouts_dir(three_d)) / (a + ".ini"));
    std::stringstream ss;
    ss << in.rdbuf();
    if (ss.str().find("[Docking]") == std::string::npos) return fail(t + ": saved layout has no docking section");
  }
  // Load the docked one from the menu: applied at the start of the next frame.
  u.harvest();
  const std::string load_a = find_action(u.actions().harvested("menu"), "Layouts/" + a);
  if (load_a.empty() || !u.run_now(load_a)) return fail(t + ": saved layout not in Window > Layouts");
  if (!ux::layout_pending()) return fail(t + ": layout load not queued");
  tick(3);
  insp = ImGui::FindWindowByName(panel);
  if (!insp || insp->DockId == 0) return fail(t + ": loading the docked layout left the Inspector floating");
  if (!ux::queue_load_layout(three_d, b, &err)) return fail(t + ": load " + b + ": " + err);
  tick(3);
  insp = ImGui::FindWindowByName(panel);
  if (!insp || insp->DockId != 0) return fail(t + ": loading the floating layout didn't float the Inspector");
  if (!u.run_now(reset_id)) return fail(t + ": Reset layout");
  tick(3);
  insp = ImGui::FindWindowByName(panel);
  if (!insp || insp->DockId == 0) return fail(t + ": Reset layout left the Inspector floating");
  (void)docked;
  // The name popup, with a path-ish name: cleaned, stays in layouts/.
  if (ux::clean_layout_name("../../evil\\name?") != "evilname") return fail("layout names aren't cleaned");
  if (!u.run_now(save_id)) return fail(t + ": Save current...");
  tick(2);
  if (!ImGui::FindWindowByName("Save layout###ts_save_layout")) return fail(t + ": Save current... didn't ask for a name");
  ctx.frames(1, [&] {
    s.on_update(1.0f / 60.0f);
    ImGui::ClosePopupToLevel(0, false);
  });
  tick(2);
  for (const std::string& n : {a, b})
    if (!ux::delete_layout(three_d, n, &err)) return fail(t + ": delete layout: " + err);
  for (const std::string& n : ux::list_layouts(three_d))
    if (n == a || n == b) return fail(t + ": deleted layout still listed");
  return 0;
}

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
  int error_cases = 0;
  if (int r = check_errors(ctx, root, &error_cases)) return r;
  // First-run tours + layouts, on fresh prefs (nothing seen yet).
  int tour2d = 0, tour3d = 0;
  {
    const bool was = ux::Walkthrough::autostart();
    ux::Walkthrough::set_autostart(true);
    ux::prefs().walkthrough_2d_done = ux::prefs().walkthrough_3d_done = false;
    int r = 0;
    {
      editor::ProjectInfo info;
      info.id = "_ux_2d";
      info.name = "UX 2D";
      info.path = (root / "p2d").string();
      editor::Editor2DScreen s(info);
      s.on_enter();
      r = check_tour_and_layouts(ctx, s, false, &tour2d);
      s.on_exit();
    }
    if (!r) {
      editor::ProjectInfo info;
      info.id = "_ux_3d";
      info.name = "UX 3D";
      info.kind = editor::ProjectKind::ThreeD;
      info.path = (root / "p3d").string();
      editor::Editor3DScreen e(info);
      e.on_enter();
      r = check_tour_and_layouts(ctx, e, true, &tour3d);
      e.on_exit();
    }
    if (!r) {
      // Seen once: a second visit doesn't start it again.
      editor::ProjectInfo info;
      info.id = "_ux_2d";
      info.name = "UX 2D";
      info.path = (root / "p2d").string();
      editor::Editor2DScreen s(info);
      s.on_enter();
      ctx.frames(8, [&] { s.on_update(1.0f / 60.0f); });
      if (s.field_guide().walkthrough().active()) r = fail("2D: walkthrough came back after it was seen");
      s.on_exit();
    }
    ux::Walkthrough::set_autostart(was);
    if (r) return r;
  }
  fs::remove_all(root, ec);
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
            << "empty states in 6 2D / 6 3D panels; readable errors for " << error_cases
            << " cases incl. a newer scene.json left untouched; walkthrough " << tour2d << " 2D / " << tour3d
            << " 3D steps, skip + don't-show-again + finish persisted; layouts save / load / reset round trip)\n";
  return 0;
}
