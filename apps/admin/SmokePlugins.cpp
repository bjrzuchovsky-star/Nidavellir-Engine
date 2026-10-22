#include "SmokePlugins.h"

#include "core/Engine.h"
#include "editor/AppFlow.h"
#include "editor/plugins/EditorPlugins.h"
#include "editor/projects/ProjectStore.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "plugin/Catalog.h"
#include "plugin/PluginManager.h"
#include "plugins/Builtins.h"
#include "plugins/TumbleweedPlugin.h"
#include "render3d/Renderer3D.h"
#include "runtime/World.h"
#include "xr/XrSession.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ts::tombstone;

namespace {

// --- Fixture plugins --------------------------------------------------------------

std::vector<std::string>* g_events = nullptr;  // "load:x" / "unload:x"

class Fixture : public plugin::Plugin {
 public:
  explicit Fixture(plugin::PluginInfo info) : info_(std::move(info)) {}
  const plugin::PluginInfo& info() const override { return info_; }
  bool on_load(const plugin::PluginContext&, std::string* error) override {
    if (info_.id == "fx.refuses") {
      *error = "no water at this camp";
      return false;
    }
    if (g_events) g_events->push_back("load:" + info_.id);
    return true;
  }
  void on_register(plugin::Registrar& reg) override {
    reg.add_component("Comp_" + info_.id, "fixture");
    if (info_.id == "fx.throws") {
      throw std::runtime_error("saddle slipped");
    }
    if (info_.id == "fx.a") {
      reg.add_asset_type({"Fixture.JSON", "fixture asset"});
      plugin::EditorMenuItem m;
      m.menu = "Frontier";
      m.label = "Survey";
      reg.add_menu_item(m);
    }
  }
  void on_unload() override {
    if (g_events) g_events->push_back("unload:" + info_.id);
  }

 private:
  plugin::PluginInfo info_;
};

plugin::PluginInfo fx(const std::string& id, plugin::Version v,
                      std::vector<plugin::Dependency> deps = {},
                      std::uint32_t types = plugin::kType2D) {
  plugin::PluginInfo i;
  i.id = id;
  i.name = id;
  i.version = v;
  i.deps = std::move(deps);
  i.project_types = types;
  return i;
}

void add(plugin::Catalog& c, plugin::PluginInfo info) {
  std::string err;
  if (!c.add_static([info]() { return std::make_unique<Fixture>(info); }, &err)) {
    throw std::runtime_error("fixture add failed: " + err);
  }
}

bool has_diag(const plugin::PluginManager& m, const std::string& id,
              runtime::LogLevel level, const std::string& needle) {
  for (const auto& d : m.diagnostics()) {
    if (d.plugin == id && d.level == level &&
        d.text.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& x : v) s += (s.empty() ? "" : ",") + x;
  return s;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

int fail(const std::string& what) {
  std::cerr << "[smoke] plugins FAILED: " << what << '\n';
  return 1;
}

// --- 1. Manager: order, deps, errors --------------------------------------------

int manager_checks() {
  using plugin::Version;
  std::vector<std::string> events;
  g_events = &events;
  int rc = 0;
  {
    plugin::Catalog c;
    // Catalog order deliberately puts dependents first.
    add(c, fx("fx.c", {1, 2, 0}, {{"fx.b", {1, 0, 0}}}));
    add(c, fx("fx.b", {1, 4, 0}, {{"fx.a", {1, 0, 0}}}));
    add(c, fx("fx.a", {1, 0, 3}));
    add(c, fx("fx.opt", {0, 1, 0}, {{"fx.ghost", {1, 0, 0}, true}, {"fx.c", {1, 0, 0}, true}}));
    add(c, fx("fx.needs-ghost", {1, 0, 0}, {{"fx.ghost", {1, 0, 0}}}));
    add(c, fx("fx.too-new", {1, 0, 0}, {{"fx.a", {2, 0, 0}}}));
    add(c, fx("fx.loop1", {1, 0, 0}, {{"fx.loop2", {1, 0, 0}}}));
    add(c, fx("fx.loop2", {1, 0, 0}, {{"fx.loop1", {1, 0, 0}}}));
    add(c, fx("fx.refuses", {1, 0, 0}));
    add(c, fx("fx.after-refuser", {1, 0, 0}, {{"fx.refuses", {1, 0, 0}}}));
    add(c, fx("fx.throws", {1, 0, 0}));
    add(c, fx("fx.vr-only", {1, 0, 0}, {}, plugin::kTypeVR));
    std::string err;
    if (c.add_static([]() { return std::make_unique<Fixture>(fx("fx.a", {9, 0, 0})); }, &err) ||
        err.find("two plugins claim") == std::string::npos) {
      g_events = nullptr;
      return fail("duplicate ids should be refused (" + err + ")");
    }
    if (c.add_static([]() { return std::make_unique<Fixture>(fx("Bad Id!", {1, 0, 0})); }, &err)) {
      g_events = nullptr;
      return fail("a bad id should be refused");
    }

    plugin::PluginManager m(c);
    std::vector<plugin::Diagnostic> telegraph;
    m.set_log_sink([&](const plugin::Diagnostic& d) { telegraph.push_back(d); });
    plugin::PluginContext ctx;
    const std::size_t n =
        m.load({"fx.opt", "fx.c", "fx.b", "fx.a", "fx.needs-ghost", "fx.too-new",
                "fx.loop1", "fx.loop2", "fx.refuses", "fx.after-refuser",
                "fx.throws", "fx.vr-only", "fx.missing"},
               ctx);
    const std::string order = join(m.load_order());
    auto check = [&](bool ok, const std::string& what) {
      if (!ok && rc == 0) rc = fail(what + " (order: " + order + ")");
    };
    check(order == "fx.a,fx.b,fx.c,fx.opt", "deps load first, optional deps order");
    check(n == 4, "four plugins should hitch up");
    // fx.throws loads, fails to register, and is unloaded again.
    check(join(events) ==
              "load:fx.a,load:fx.b,load:fx.c,load:fx.opt,load:fx.throws,unload:fx.throws",
          "on_load runs in dependency order: " + join(events));
    check(has_diag(m, "fx.needs-ghost", runtime::LogLevel::Error, "isn't installed"),
          "missing dependency error");
    check(has_diag(m, "fx.too-new", runtime::LogLevel::Error, "found 1.0.3"),
          "version mismatch error");
    check(has_diag(m, "fx.loop1", runtime::LogLevel::Error, "dependency loop") &&
              has_diag(m, "fx.loop2", runtime::LogLevel::Error, "dependency loop"),
          "dependency loop error");
    check(has_diag(m, "fx.refuses", runtime::LogLevel::Error, "no water at this camp"),
          "on_load refusal error");
    check(has_diag(m, "fx.after-refuser", runtime::LogLevel::Error, "failed to load"),
          "dependents of a refused plugin are skipped");
    check(has_diag(m, "fx.throws", runtime::LogLevel::Error, "saddle slipped"),
          "a throwing on_register is contained");
    check(has_diag(m, "fx.vr-only", runtime::LogLevel::Warn, "doesn't work in 2D"),
          "project type mismatch warning");
    check(has_diag(m, "fx.missing", runtime::LogLevel::Warn, "isn't in the"),
          "unknown enabled id warning");
    check(telegraph.size() == m.diagnostics().size(), "every diagnostic reaches the sink");
    const auto ext = m.world_extensions();
    bool throws_rolled_back = true;
    for (const auto& comp : ext->components) {
      throws_rolled_back &= comp.owner != "fx.throws";
    }
    check(ext->components.size() == 4 && throws_rolled_back,
          "contributions of a failed register are rolled back");
    check(m.asset_type_for("maps/town.fixture.json") != nullptr &&
              m.asset_type_for("maps/town.fixture.json")->owner == "fx.a",
          "asset types match by suffix, case-insensitive");
    check(m.menu_items().size() == 1 && m.menu_items()[0].item.menu == "Frontier",
          "menu contributions are collected");

    // Resolve-only: a disabled dependency.
    const plugin::Resolution r = m.resolve({"fx.c"}, plugin::ProjectType::TwoD);
    bool told = false;
    for (const auto& d : r.diagnostics) {
      told |= d.plugin == "fx.c" && d.text.find("switch it on too") != std::string::npos;
    }
    check(r.order.empty() && told, "a disabled dependency is reported, not auto-loaded");

    events.clear();
    m.unload_all();
    check(join(events) == "unload:fx.opt,unload:fx.c,unload:fx.b,unload:fx.a",
          "unload runs in reverse order: " + join(events));
    check(m.world_extensions()->empty(), "unload drops contributions");
  }
  g_events = nullptr;
  if (rc) return rc;

  // Versions.
  plugin::Version v;
  if (!plugin::Version::parse("1.4.2-beta", &v) || v.str() != "1.4.2" ||
      plugin::Version::parse("1..2", nullptr) ||
      !plugin::Version{1, 4, 0}.satisfies({1, 2, 0}) ||
      plugin::Version{2, 0, 0}.satisfies({1, 2, 0}) ||
      plugin::Version{1, 1, 0}.satisfies({1, 2, 0})) {
    return fail("version parse / satisfies rules");
  }
  if (plugin::split_plugin_list(" a, b,,a ,c") != std::vector<std::string>{"a", "b", "c"} ||
      plugin::join_plugin_list({"a", "b"}) != "a,b") {
    return fail("plugin list codec");
  }
  return 0;
}

// --- 2. Built-ins, dynamic loading, World integration -------------------------

int builtin_checks() {
  plugin::Catalog c;
  std::vector<std::string> errors;
  plugins::register_builtin_plugins(c, &errors);
  editor::register_editor_builtin_plugins(c, &errors);
  if (!errors.empty()) return fail("builtin registration: " + errors[0]);
  if (!c.find("ts.tumbleweed") || !c.find("ts.posse") ||
      c.find("ts.tumbleweed")->source != "built-in") {
    return fail("TS_PLUGIN_STATIC built-ins should be in the catalog");
  }
  // Dynamic sample next to the build.
  std::string err;
  if (c.load_library("no/such/plugin" + std::string(".so"), &err) ||
      err.find("could not open") == std::string::npos) {
    return fail("a missing library should be a clear error: " + err);
  }
  const fs::path junk = fs::path("TombStoneProjects") / "_junk_plugin.so";
  fs::create_directories(junk.parent_path());
  { std::ofstream(junk) << "not a library"; }
  if (c.load_library(junk.string(), &err)) return fail("junk should not load as a plugin");
  fs::remove(junk);
  bool dynamic_ok = false;
#if defined(TS_SAMPLE_PLUGIN_DIR)
  std::vector<std::string> scan_errors;
  if (c.scan_folder(TS_SAMPLE_PLUGIN_DIR, &scan_errors) < 1 || !c.find("sample.lantern")) {
    return fail(std::string("sample dynamic plugin should load from ") + TS_SAMPLE_PLUGIN_DIR +
                (scan_errors.empty() ? "" : ": " + scan_errors[0]));
  }
  if (!c.find("sample.lantern")->dynamic) return fail("lantern should be marked dynamic");
  dynamic_ok = true;
#endif

  plugin::PluginManager m(c);
  plugin::PluginContext ctx;
  if (dynamic_ok) {
    m.load({"sample.lantern"}, ctx);
    if (m.loaded("sample.lantern") ||
        !has_diag(m, "sample.lantern", runtime::LogLevel::Error, "switch it on too")) {
      return fail("lantern without tumbleweeds should be refused with a clear error");
    }
  }
  std::vector<std::string> want{"ts.tumbleweed"};
  if (dynamic_ok) want.push_back("sample.lantern");
  m.load(want, ctx);
  if (!m.loaded("ts.tumbleweed") || (dynamic_ok && m.load_order().back() != "sample.lantern")) {
    return fail("tumbleweed (static) then lantern (dynamic) should hitch up: " +
                join(m.load_order()));
  }

  // World: a tumbleweed rolls, scripts see the Lua table.
  runtime::World w;
  w.set_extensions(m.world_extensions());
  std::vector<Entity2D> ents;
  Entity2D weed;
  weed.id = 1;
  weed.name = "Tumbleweed Big";
  weed.x = 100.0f;
  weed.y = 200.0f;
  weed.w = 16.0f;
  weed.h = 16.0f;
  ents.push_back(weed);
  Entity2D post;
  post.id = 2;
  post.name = "Wind Post";
  post.script = ScriptData{};
  post.script->path = "scripts/wind.lua";
  ents.push_back(post);
  w.script_library().put("scripts/wind.lua",
                         "function on_start()\n"
                         "  tumbleweed.wind(120)\n"
                         "  log('weeds=' .. tumbleweed.count() .. ' wind=' .. tumbleweed.wind())\n"
                         "end\n");
  std::string build_err;
  if (!w.build(ents, std::string(), &build_err)) return fail("world build: " + build_err);
  m.play_begin(w);
  runtime::InputFrame input;
  for (int i = 0; i < 60; ++i) w.step(input);
  m.play_end();
  const runtime::Actor* a = w.find(1);
  bool lua_ok = false;
  std::string last_log;
  for (const auto& l : w.take_logs()) {
    last_log = l.text;
    lua_ok |= l.text.find("wind=120") != std::string::npos;
  }
  // 1 tick at 40 px/s, then 59 at 120 px/s (on_start runs at the start of tick 0).
  if (!a || a->data.x < 100.0f + 100.0f || a->data.x > 100.0f + 125.0f) {
    return fail("the tumbleweed system should roll the weed with the scripted wind (x=" +
                std::to_string(a ? a->data.x : -1.0f) + ")");
  }
  if (!lua_ok) return fail("scripts should see the tumbleweed Lua table (" + last_log + ")");
  auto* tw = static_cast<plugins::TumbleweedPlugin*>(m.get("ts.tumbleweed"));
  if (!tw || tw->gusts() != 60) return fail("the system should run once per tick");
  if (!m.asset_type_for("spawns/east.tumble.json")) return fail("tumbleweed asset type");
  w.clear();
  w.set_extensions(nullptr);
  m.unload_all();
  std::cout << "[smoke] plugins: built-ins + "
            << (dynamic_ok ? "dynamic sample.lantern" : "(no dynamic sample)")
            << " OK, tumbleweed system + Lua table ride in a World\n";
  return 0;
}

// --- 3. Project files: type round trip, old projects, persistence -------------

int project_checks() {
  const fs::path root = fs::path("TombStoneProjects") / "_plugin_smoke_root";
  std::error_code ec;
  fs::remove_all(root, ec);
  editor::ProjectStore store(root.string());
  if (!store.refresh()) return fail("store refresh: " + store.last_error());

  for (editor::ProjectKind k : {editor::ProjectKind::TwoD, editor::ProjectKind::ThreeD,
                                editor::ProjectKind::VR}) {
    const std::string name = std::string("Roundtrip ") + editor::to_string(k);
    const std::vector<std::string> plugins = editor::EditorPlugins::default_ids(k);
    editor::ProjectInfo made;
    if (!store.create_project(name, k, &plugins, &made)) return fail(store.last_error());
    editor::ProjectInfo back;
    std::string err;
    if (!editor::ProjectStore::load_project_json(made.path, &back, &err)) return fail(err);
    const editor::ProjectKind want =
        k == editor::ProjectKind::VR ? editor::ProjectKind::ThreeD : k;
    if (back.kind != want || back.vr != (k == editor::ProjectKind::VR) || !back.plugins_set ||
        back.plugins != plugins) {
      return fail(std::string("project type / plugins should round-trip for ") +
                  editor::to_string(k));
    }
    if (k == editor::ProjectKind::TwoD &&
        std::find(plugins.begin(), plugins.end(), "ts.posse") == plugins.end()) {
      return fail("new 2D projects start with the posse");
    }
    if (k != editor::ProjectKind::TwoD && !plugins.empty()) {
      return fail("3D / VR projects start with no 2D-only plugins");
    }
  }

  // VR moved from a dimension to a 3D switch: an old "dimension": "vr" file
  // loads as 3D + VR, and the project-settings toggle rewrites it as 3d.
  {
    const fs::path legacy = root / "legacy-goggles";
    fs::create_directories(legacy);
    std::ofstream(legacy / "project.json")
        << "{\n  \"name\": \"Legacy Goggles\",\n  \"dimension\": \"vr\",\n"
           "  \"created\": \"2026-01-02\",\n  \"window_width\": \"1600\"\n}\n";
    editor::ProjectInfo lg;
    std::string lerr;
    if (!editor::ProjectStore::load_project_json(legacy.string(), &lg, &lerr) ||
        lg.kind != editor::ProjectKind::ThreeD || !lg.vr || !editor::vr_enabled(lg)) {
      return fail("a legacy vr project should load as 3D with VR on");
    }
    if (!editor::ProjectStore::set_project_vr(legacy.string(), false, &lerr)) return fail(lerr);
    editor::ProjectStore::load_project_json(legacy.string(), &lg, &lerr);
    const std::string txt = read_file(legacy / "project.json");
    if (lg.vr || lg.kind != editor::ProjectKind::ThreeD ||
        txt.find("\"3d\"") == std::string::npos || txt.find("1600") == std::string::npos) {
      return fail("VR off should write dimension 3d and keep other fields: " + txt);
    }
    if (!editor::ProjectStore::set_project_vr(legacy.string(), true, &lerr)) return fail(lerr);
    editor::ProjectStore::load_project_json(legacy.string(), &lg, &lerr);
    if (!lg.vr) return fail("VR back on");
    lg.kind = editor::ProjectKind::TwoD;
    editor::ProjectInfo two;
    two.path = (root / "two-d-vr").string();
    two.name = "Flat";
    two.vr = true;
    editor::ProjectStore::write_project_json(two, &lerr);
    editor::ProjectStore::load_project_json(two.path, &two, &lerr);
    if (two.vr || editor::ProjectStore::set_project_vr(two.path, true, &lerr)) {
      return fail("2D projects have no VR switch");
    }
    fs::remove_all(legacy);
    fs::remove_all(two.path);
  }

  // A project.json from before plugins and project types.
  const fs::path old_dir = root / "old-claim";
  fs::create_directories(old_dir);
  {
    std::ofstream(old_dir / "project.json")
        << "{\n  \"name\": \"Old Claim\",\n  \"created\": \"2026-01-02 03:04:05\"\n}\n";
  }
  editor::ProjectInfo old;
  std::string err;
  if (!editor::ProjectStore::load_project_json(old_dir.string(), &old, &err)) return fail(err);
  if (old.kind != editor::ProjectKind::TwoD || old.plugins_set) {
    return fail("an old project should load as 2D with default plugins");
  }

  editor::EditorPlugins ep;
  std::vector<std::string> said;
  ep.open(&old, "smoke", nullptr,
          [&](runtime::LogLevel, const std::string& t) { said.push_back(t); });
  if (ep.enabled() != std::vector<std::string>{"ts.posse"} || !ep.loaded("ts.posse")) {
    return fail("old project defaults: " + join(ep.enabled()));
  }
  if (read_file(old_dir / "project.json").find("plugins") != std::string::npos) {
    return fail("opening an old project should not rewrite it");
  }
  const bool has_lantern = ep.catalog().find("sample.lantern") != nullptr;
  if (has_lantern) {
    // Lantern needs tumbleweeds: switching it on hitches both.
    if (!ep.set_enabled("sample.lantern", true) || !ep.is_enabled("ts.tumbleweed")) {
      return fail("enabling a plugin should enable its dependencies");
    }
  } else if (!ep.set_enabled("ts.tumbleweed", true)) {
    return fail("enable tumbleweed");
  }
  if (!ep.pending() || ep.loaded("ts.tumbleweed")) {
    return fail("a change waits for apply (restart-of-play)");
  }
  ep.apply();
  if (!ep.loaded("ts.tumbleweed") || ep.pending()) return fail("apply should hitch it up");

  editor::ProjectInfo reread;
  editor::ProjectStore::load_project_json(old_dir.string(), &reread, &err);
  const std::string json = read_file(old_dir / "project.json");
  if (!reread.plugins_set || std::find(reread.plugins.begin(), reread.plugins.end(),
                                       "ts.tumbleweed") == reread.plugins.end() ||
      reread.name != "Old Claim" || reread.created != "2026-01-02 03:04:05") {
    return fail("enable should persist in project.json: " + json);
  }
  if (!ep.set_enabled("ts.tumbleweed", false) ||
      (has_lantern && ep.is_enabled("sample.lantern"))) {
    return fail("disabling a plugin should disable what needs it");
  }
  if (!ep.set_enabled("ts.posse", false)) return fail("disable posse");
  editor::ProjectStore::load_project_json(old_dir.string(), &reread, &err);
  if (!reread.plugins_set || !reread.plugins.empty()) {
    return fail("an empty plugin list should persist as empty (not defaults)");
  }
  // A plugin id nobody has (removed DLL) stays in the list, with a warning.
  reread.plugins = {"frontier.ghost"};
  editor::ProjectStore::write_project_json(reread, &err);
  editor::ProjectInfo ghost;
  editor::ProjectStore::load_project_json(old_dir.string(), &ghost, &err);
  editor::EditorPlugins ep2;
  std::vector<std::string> said2;
  ep2.open(&ghost, "smoke", nullptr,
           [&](runtime::LogLevel, const std::string& t) { said2.push_back(t); });
  bool warned = false;
  for (const auto& s : said2) warned |= s.find("frontier.ghost") != std::string::npos;
  if (!warned || !ep2.is_enabled("frontier.ghost")) {
    return fail("a missing plugin should warn and stay listed");
  }
  ep2.close();
  ep.close();
  fs::remove_all(root, ec);
  std::cout << "[smoke] plugins: project type 2D/3D/VR round trip, old project.json "
               "(2D + default plugins, untouched until changed), enable/disable "
               "persistence with deps + dependents OK\n";
  return 0;
}

// --- 4. Editor: posse as a plugin, restart-of-play ---------------------------

int editor_checks() {
  const fs::path dir = fs::path("TombStoneProjects") / "_plugin_editor_smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  editor::ProjectInfo info;
  info.id = "_plugin_editor_smoke";
  info.name = "Plugin Smoke";
  info.path = dir.string();
  editor::Editor2DScreen scr(info);
  scr.on_enter();
  auto panels_of = [&](const std::string& owner) {
    int n = 0;
    for (const auto& p : scr.plugins().manager().editor_panels()) n += p.owner == owner;
    return n;
  };
  if (!scr.plugins().loaded("ts.posse") || panels_of("ts.posse") != 2) {
    return fail("the posse plugin should contribute the Network and Town Hall panels");
  }
  if (!scr.set_plugin_enabled("ts.posse", false) || scr.plugins().loaded("ts.posse") ||
      panels_of("ts.posse") != 0 || !scr.plugins().manager().menu_items().empty()) {
    return fail("unhitching the posse should drop its panel and menu entries at once");
  }
  if (scr.start_posse() || scr.posse_running()) {
    return fail("no posse ride without the posse plugin");
  }
  scr.set_plugin_enabled("ts.posse", true);
  scr.set_plugin_enabled("ts.tumbleweed", true);
  if (!scr.plugins().loaded("ts.posse") || !scr.plugins().loaded("ts.tumbleweed")) {
    return fail("re-hitching while idle applies at once");
  }
  const std::uint64_t id = scr.create_entity("Tumbleweed");
  const float x0 = scr.workspace().find(id)->x;
  if (!scr.start_play()) return fail("Play");
  runtime::InputFrame input;
  for (int i = 0; i < 10; ++i) scr.update_play(1.0 / 60.0, input);
  const runtime::Actor* a = scr.play_session().world().find(id);
  if (!a || a->data.x <= x0) return fail("plugin systems should ride in editor Play");
  // Toggle mid-ride: saved now, applied when the ride ends.
  scr.set_plugin_enabled("ts.tumbleweed", false);
  if (!scr.plugins().loaded("ts.tumbleweed") || !scr.plugins().pending()) {
    return fail("a change during Play should wait for the ride to end");
  }
  scr.stop_play();
  if (scr.plugins().loaded("ts.tumbleweed") || scr.plugins().pending()) {
    return fail("stopping Play should apply the waiting change");
  }
  if (scr.workspace().find(id)->x != x0) return fail("Play must not move the edit scene");
  editor::ProjectInfo disk;
  std::string err;
  editor::ProjectStore::load_project_json(dir.string(), &disk, &err);
  // The seeded rider switched on Top-down Rider (0.2.0 split).
  if (disk.plugins != std::vector<std::string>{"ts.move.topdown", "ts.posse"} &&
      disk.plugins != std::vector<std::string>{"ts.posse", "ts.move.topdown"}) {
    return fail("the editor should persist the list: " + join(disk.plugins));
  }
  scr.on_exit();
  fs::remove_all(dir, ec);
  std::cout << "[smoke] plugins: editor posse-as-plugin (panel/menus/toolbar follow the "
               "plugin, start refused when off), tumbleweeds in Play, restart-of-play "
               "apply on Stop OK\n";
  return 0;
}

// --- 5. Create-project flow + 3D / VR editor --------------------------------------

int flow_checks() {
  Engine engine;
  engine.init();
  editor::AppFlow flow;
  flow.start();
  for (int i = 0; i < 200 && flow.current_state() == editor::AppState::Loading; ++i) {
    engine.tick(1.0f / 60.0f);
    flow.tick(1.0f / 60.0f);
  }
  if (flow.current_state() == editor::AppState::Login) flow.submit_dev_login();
  if (flow.current_state() != editor::AppState::ProjectManager) {
    return fail("expected the Project Manager");
  }
  auto find = [&](const std::string& name) -> int {
    for (std::size_t i = 0; i < flow.projects().size(); ++i) {
      if (flow.projects()[i].name == name) return static_cast<int>(i);
    }
    return -1;
  };
  struct Case {
    editor::ProjectKind kind;
    const char* name;
  };
  for (const Case& c : {Case{editor::ProjectKind::VR, "Goggle Gulch"},
                        Case{editor::ProjectKind::ThreeD, "Mesa Verde 3D"}}) {
    if (!flow.create_new_project(c.name, c.kind)) return fail("create " + std::string(c.name));
    const int idx = find(c.name);
    // VR is a 3D switch now: a "VR" create is 3D with vr on.
    if (idx < 0 || flow.projects()[idx].kind != editor::ProjectKind::ThreeD ||
        flow.projects()[idx].vr != (c.kind == editor::ProjectKind::VR)) {
      return fail("created kind (3D, VR switch)");
    }
    if (!flow.select_project(static_cast<std::size_t>(idx))) return fail("open " + std::string(c.name));
    editor::Editor3DScreen* ed = flow.editor3d_screen();
    if (flow.current_state() != editor::AppState::Editor3D || !ed) {
      return fail("3D and VR projects open the 3D editor");
    }
    if (ed->scene().entities.empty() || std::string(ed->renderer_backend()) != "null" ||
        ed->render_frame(64, 64).draws == 0) {
      return fail("the 3D editor starts with a starter scene drawn through ts_render3d");
    }
    bool has_rig = false;
    for (const auto& e : ed->scene().entities) has_rig = has_rig || e.vr_rig.has_value();
    if (has_rig != (c.kind == editor::ProjectKind::VR)) {
      return fail("only VR claims start with a VR rig");
    }
    if (c.kind == editor::ProjectKind::VR) {
      // Play in a VR claim = the desktop simulator, headless here.
      if (!ed->play_start() || !ed->vr_previewing() || !ed->vr_sim() || !ed->world().vr()) {
        return fail("VR Play should start the simulator preview");
      }
      for (int i = 0; i < 10; ++i) ed->vr_preview_frame(1.0 / 60.0);
      if (ed->world().tick() < 5 || ed->world().vr_rig_id() == 0) {
        return fail("the VR preview should tick the world with the rig");
      }
      ed->play_stop();
      if (ed->vr_previewing() || ed->playing()) return fail("Stop ends the VR preview");
    }
    flow.request_back_to_projects();
    if (flow.current_state() != editor::AppState::ProjectManager) {
      return fail("back to claims from the 3D editor");
    }
  }
  if (!flow.create_new_project("Dusty Town", editor::ProjectKind::TwoD)) return fail("create 2D");
  const int idx = find("Dusty Town");
  if (idx < 0 || !flow.select_project(static_cast<std::size_t>(idx)) ||
      flow.current_state() != editor::AppState::Editor2D) {
    return fail("2D projects open the 2D editor as before");
  }
  flow.request_back_to_projects();
  flow.request_quit();
  engine.shutdown();

  // Interfaces directly.
  auto r = render3d::make_null_renderer3d();
  std::string err;
  r->init(&err);
  render3d::MeshData bad;
  bad.vertices.resize(3);
  bad.indices = {0, 1, 7};
  if (r->create_mesh(bad).id != 0 || !render3d::make_cube().valid()) {
    return fail("mesh validation");
  }
  auto xs = xr::create_sim_session({});
  if (!xs->begin(&err) || !xs->running() || !xs->simulated()) {
    return fail("the VR simulator should begin anywhere: " + err);
  }
  xs->end();
  std::cout << "[smoke] plugins: create 2D / 3D / VR from the Project Manager flow, "
               "3D editor for 3D + VR claims (VR rig, simulator preview) OK\n";
  return 0;
}

}  // namespace

int run_plugin_smoke() {
  if (manager_checks() != 0) return 1;
  std::cout << "[smoke] plugins: load order, deps, missing/disabled/version/loop/"
               "refusal/throw errors, type filter, unload order OK\n";
  if (builtin_checks() != 0) return 1;
  if (project_checks() != 0) return 1;
  if (editor_checks() != 0) return 1;
  if (flow_checks() != 0) return 1;
  return 0;
}
