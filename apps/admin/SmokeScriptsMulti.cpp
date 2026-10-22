#include "SmokeScriptsMulti.h"

#include "SmokeGraph.h"
#include "editor/ProjectInfo.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/scripting/ScriptComponents.h"
#include "editor/workspace/SceneIO.h"
#include "render3d/Scene3D.h"
#include "runtime/CSharpScript.h"
#include "runtime/GraphScript.h"
#include "runtime/Log.h"
#include "runtime/NativeScript.h"
#include "runtime/ScriptHost.h"
#include "runtime/World.h"
#include "scene/SampleScripts.h"
#include "scene/SceneData.h"
#include "scene/SceneJson.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ts::tombstone;
using namespace ts::tombstone::editor;
namespace fs = std::filesystem;

int fail(const std::string& what) {
  std::cerr << "[smoke] scripts multi FAILED: " << what << '\n';
  return 1;
}

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_text(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

std::string dump(const std::vector<runtime::LogEntry>& logs) {
  std::string s;
  for (const runtime::LogEntry& e : logs) s += "\n  " + runtime::format_log(e, true);
  return s;
}

// Index of the first log line containing `text` (-1 none).
int find_log(const std::vector<runtime::LogEntry>& logs, const std::string& text) {
  for (std::size_t i = 0; i < logs.size(); ++i) {
    if (has(runtime::format_log(logs[i], true), text)) return static_cast<int>(i);
  }
  return -1;
}

std::vector<std::string> paths(const std::vector<ScriptData>& stack) {
  std::vector<std::string> out;
  for (const ScriptData& s : stack) out.push_back(s.path);
  return out;
}

// Replace the number after "version": in scene text.
std::string with_version(std::string text, int v) {
  const std::size_t k = text.find("\"version\"");
  if (k == std::string::npos) return text;
  std::size_t a = text.find_first_of("0123456789", k);
  std::size_t b = text.find_first_not_of("0123456789", a);
  return text.replace(a, b - a, std::to_string(v));
}

// --- 1. Templates in every language --------------------------------------------

int check_templates(const fs::path& proj, bool cpp_ok, bool cs_ok, std::string* note) {
  std::error_code ec;
  fs::remove_all(proj, ec);
  fs::create_directories(proj, ec);
  const auto& templates = script_templates();
  if (templates.size() < 5) return fail("five templates (Blank, Gate, Pickup, Sign, Ticker)");
  std::vector<Entity2D> cpp_ents, cs_ents;
  std::uint64_t next = 10;
  for (int l = 0; l < kScriptLangCount; ++l) {
    const ScriptLang lang = static_cast<ScriptLang>(l);
    for (const ScriptTemplateInfo& t : templates) {
      static const char* kSuffix[] = {"lua", "graph", "cpp", "cs"};
      const std::string name = std::string(t.id) + " " + kSuffix[l];
      const NewScriptResult r = create_script_file(proj.string(), name, lang, t.id);
      if (!r.ok || !fs::exists(proj / r.rel)) return fail("New Script " + name + ": " + r.message);
      if (r.rel.rfind("scripts/", 0) != 0 || lang_of(r.rel) != lang) return fail("New Script path " + r.rel);
      const std::string src = read_text(proj / r.rel);
      std::vector<ScriptProp> props;
      std::string err;
      if (!runtime::ScriptHost::describe(src, r.rel, &props, &err)) return fail(r.rel + " does not describe: " + err);
      if (lang == ScriptLang::Graph) {
        runtime::Graph g;
        std::vector<std::string> problems;
        if (!runtime::parse_graph(src, &g, &err)) return fail(r.rel + " does not parse: " + err);
        if (!runtime::check_graph(g, &problems) || !problems.empty()) {
          return fail(r.rel + " graph problems: " + (problems.empty() ? std::string("?") : problems[0]));
        }
      }
      Entity2D e;
      e.id = next++;
      e.name = name;
      e.script = ScriptData{};
      e.script->path = r.rel;
      if (lang == ScriptLang::Cpp) cpp_ents.push_back(e);
      if (lang == ScriptLang::CSharp) cs_ents.push_back(e);
    }
  }
  // Refusals: same stem in another language, bad names, unknown template.
  if (create_script_file(proj.string(), "blank lua", ScriptLang::Cpp, "blank").ok)
    return fail("a stem taken by another language should be refused");
  if (create_script_file(proj.string(), "!!!", ScriptLang::Lua, "blank").ok) return fail("'!!!' should be refused");
  if (create_script_file(proj.string(), "fresh", ScriptLang::Lua, "nope").ok)
    return fail("an unknown template should be refused");
  if (list_script_files(proj.string()).size() != templates.size() * kScriptLangCount)
    return fail("list_script_files should list every language");

  // C++ / C# templates compile against the SDK (blocking builds).
  runtime::native_scripts::set_blocking_builds(true);
  struct Unblock { ~Unblock() { runtime::native_scripts::set_blocking_builds(false); } } unblock;
  auto build = [&](const std::vector<Entity2D>& ents, const char* what) -> int {
    runtime::World w;
    w.set_extensions(smoke_plugin_extensions());
    std::string err;
    const bool ok = w.build(ents, proj.string(), &err);
    std::vector<runtime::LogEntry> logs = w.take_logs();
    for (int i = 0; i < 30; ++i) {
      w.step(runtime::InputFrame{});
      for (runtime::LogEntry& e : w.take_logs()) logs.push_back(std::move(e));
    }
    for (const runtime::LogEntry& e : logs) {
      if (e.level == runtime::LogLevel::Error) return fail(std::string(what) + " templates: " + dump(logs));
    }
    if (!ok) return fail(std::string(what) + " templates did not build: " + err + dump(logs));
    for (const Entity2D& e : ents) {
      if (!w.scripts()->running(e.id)) return fail(std::string(what) + " template not running: " + e.script->path);
    }
    return 0;
  };
  std::string said;
  if (cpp_ok) {
    if (int r = build(cpp_ents, "C++")) return r;
    said += "C++ templates compiled";
  } else {
    said += "C++ templates written (no compiler)";
  }
  if (cs_ok) {
    if (int r = build(cs_ents, "C#")) return r;
    said += ", C# templates compiled";
  } else {
    said += ", C# templates create-only (no .NET SDK)";
  }
  *note = said;
  return 0;
}

// --- 2. The 2D editor -------------------------------------------------------------

int check_editor_2d(bool cs_ok, std::string* note) {
  const fs::path dir = fs::path("TombStoneProjects") / "_multi_script_smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  ProjectInfo info;
  info.id = "_multi_script_smoke";
  info.name = "Multi Script Smoke";
  info.path = dir.string();
  Editor2DScreen scr(info);
  scr.on_enter();
  Workspace2D& w = scr.workspace();
  const fs::path scene = scene_io::scene_path_for_project(info.path);
  const std::uint64_t wagon = scr.create_entity("Wagon");
  w.select(wagon);

  // New Script... in every language attaches in order, one undo step each.
  struct Want {
    const char* name;
    ScriptLang lang;
    const char* templ;
  };
  const Want wants[] = {{"Wagon Brain", ScriptLang::Lua, "ticker"},
                        {"Wagon Sign", ScriptLang::Graph, "sign"},
                        {"Wagon Gate", ScriptLang::Cpp, "gate"},
                        {"Wagon Pickup", ScriptLang::CSharp, "pickup"}};
  std::vector<std::string> rels;
  for (const Want& wt : wants) {
    const std::size_t undo0 = w.undo_count();
    std::string rel;
    if (!scr.new_script(wagon, wt.name, wt.lang, wt.templ, &rel)) return fail(std::string("New Script ") + wt.name);
    if (!fs::exists(dir / rel) || w.undo_count() != undo0 + 1 || w.undo_label() != "Attach Script")
      return fail("New Script should write " + rel + " and attach it as one undo step");
    rels.push_back(rel);
  }
  if (paths(script_stack(*w.find(wagon))) != rels) return fail("stack order after New Script");
  if (!has(read_text(scene), "\"scripts\"")) return fail("a stack should autosave as v12 \"scripts\"");
  if (!scr.undo() || script_count(*w.find(wagon)) != 3 || !scr.redo() || script_count(*w.find(wagon)) != 4)
    return fail("undo / redo of an attach");

  // Duplicates and a full stack are refused; drag-attach of an existing file.
  if (scr.attach_script(wagon, rels[0])) return fail("attaching the same script twice should be refused");
  if (!scr.attach_script(wagon, sample_scripts::kGate)) return fail("drag-attach gate.lua");
  rels.push_back(sample_scripts::kGate);
  if (paths(script_stack(*w.find(wagon))) != rels) return fail("drag-attach appends");

  // Reorder: gate to the top (one step), undo puts it back.
  std::vector<ScriptData> stack = script_stack(*w.find(wagon));
  std::rotate(stack.begin(), stack.end() - 1, stack.end());
  if (!scr.set_scripts(wagon, stack, "Move Script") || w.undo_label() != "Move Script" ||
      script_stack(*w.find(wagon))[0].path != sample_scripts::kGate)
    return fail("reorder");
  if (!scr.undo() || script_stack(*w.find(wagon))[0].path != rels[0] || !scr.redo()) return fail("undo reorder");

  // Per-script props: the gate slot keeps its own overrides.
  stack = script_stack(*w.find(wagon));
  stack[0].set("open_line", ScriptValue::of_text("Swing wide, Wagon."));
  if (!scr.set_scripts(wagon, stack, "Script Prop") || !has(read_text(scene), "Swing wide, Wagon."))
    return fail("per-script prop override saved");
  // Disable: kept in the stack, written with "enabled": false.
  stack = script_stack(*w.find(wagon));
  stack[2].enabled = false;
  if (!scr.set_scripts(wagon, stack, "Disable Script") || !has(read_text(scene), "\"enabled\": false"))
    return fail("disable writes enabled false");
  // Remove the graph; undo brings it back.
  stack = script_stack(*w.find(wagon));
  stack.erase(stack.begin() + 2);
  if (!scr.set_scripts(wagon, stack, "Remove Script") || script_count(*w.find(wagon)) != 4) return fail("remove");
  if (!scr.undo() || script_count(*w.find(wagon)) != 5) return fail("undo remove");

  // scene.json on disk round-trips the whole stack.
  scene_json::SceneDoc doc;
  std::string err;
  if (!scene_json::parse(read_text(scene), &doc, &err)) return fail("scene.json parse: " + err);
  const Entity2D* back = find_entity_named(doc.entities, "Wagon");
  if (!back || script_stack(*back) != script_stack(*w.find(wagon))) return fail("scene.json stack round trip");

  // IDE files + launch plans (dry run: nothing starts).
  IdeFiles ide;
  if (!write_ide_files(info.path, info.name, &ide, &err)) return fail("IDE files: " + err);
  for (const char* f : {".vscode/settings.json", ".vscode/extensions.json", "compile_commands.json"}) {
    if (!fs::exists(dir / f)) return fail(std::string("IDE file missing: ") + f);
  }
  if (ide.workspace.empty() || !fs::exists(ide.workspace)) return fail("VS Code workspace file");
  if (ide.csproj.empty() || !fs::exists(ide.csproj) || !has(read_text(ide.csproj), "scripts"))
    return fail("C# project for the IDE");
  if (!has(read_text(dir / "compile_commands.json"), "wagon_gate.cpp")) return fail("compile_commands lists C++ scripts");
  set_external_launch_dry_run(true);
  struct Undry { ~Undry() { set_external_launch_dry_run(false); } } undry;
  std::string msg;
  override_editor_install(ExternalEditor::VSCode, "/opt/fake/code");
  if (!open_script_external(ExternalEditor::VSCode, info.path, info.name, rels[2], &msg) ||
      last_external_launch().program != "/opt/fake/code" || last_external_launch().args.size() < 3 ||
      last_external_launch().args[1] != "-g" || !has(last_external_launch().args[2], "wagon_gate.cpp"))
    return fail("VS Code plan: " + msg);
  override_editor_install(ExternalEditor::VisualStudio, "C:/VS/Common7/IDE/devenv.exe");
  if (!open_script_external(ExternalEditor::VisualStudio, info.path, info.name, rels[3], &msg) ||
      last_external_launch().args.size() < 3 || last_external_launch().args[1] != "/Edit" ||
      !has(last_external_launch().args[0], ".csproj") || !has(last_external_launch().args[2], "wagon_pickup.cs"))
    return fail("Visual Studio plan (C# opens the scripts project): " + msg);
  override_editor_install(ExternalEditor::VSCode, "");
  override_editor_install(ExternalEditor::VisualStudio, "");
  if (open_script_external(ExternalEditor::VSCode, info.path, info.name, rels[0], &msg) || !has(msg, "VS Code") ||
      !has(msg, "Settings"))
    return fail("missing VS Code should fail with a readable line: " + msg);
  if (!open_script_external(ExternalEditor::Auto, info.path, info.name, rels[0], &msg) ||
      last_external_launch().program != "(os)")
    return fail("Auto without VS Code / Visual Studio falls back to the OS: " + msg);
  if (!open_script_external(ExternalEditor::Auto, info.path, info.name, rels[1], &msg) || !has(msg, "Brand Book"))
    return fail("graphs open in the Brand Book: " + msg);
  find_vscode(true);
  find_visual_studio(true);

  // Play: the stack rides (C# only with the SDK), then Stop restores.
  if (!cs_ok) {
    stack = script_stack(*w.find(wagon));
    stack.erase(stack.begin() + 3);
    scr.set_scripts(wagon, stack, "Remove Script");
  }
  scr.on_exit();
  *note = "4 languages via New Script, drag-attach, reorder / props / disable / remove + undo, IDE files, launch plans";
  return 0;
}

// --- 3. Old single-script scenes ---------------------------------------------------

int check_migration() {
  // 2D: a v11 file with the v6 "script" object.
  scene_json::SceneDoc doc;
  Entity2D post;
  post.id = 7;
  post.name = "Post";
  post.script = ScriptData{};
  post.script->path = "scripts/post.lua";
  post.script->set("reach", ScriptValue::of_number(3));
  doc.entities.push_back(post);
  const std::string now = scene_json::write(doc);
  if (!has(now, "\"script\"") || has(now, "\"scripts\"")) return fail("one script should still save as \"script\"");
  scene_json::SceneDoc old;
  std::string err;
  if (!scene_json::parse(with_version(now, 11), &old, &err) || old.entities.size() != 1 ||
      script_count(old.entities[0]) != 1 || script_stack(old.entities[0]) != script_stack(post))
    return fail("v11 single script loads as a one-script stack: " + err);
  // Adding a second script upgrades the file; it reads back the same.
  std::vector<ScriptData> stack = script_stack(old.entities[0]);
  ScriptData two;
  two.path = "scripts/two.cs";
  two.enabled = false;
  stack.push_back(two);
  set_script_stack(old.entities[0], stack);
  scene_json::SceneDoc again;
  if (!scene_json::parse(scene_json::write(old), &again, &err) || script_stack(again.entities[0]) != stack)
    return fail("2D stack round trip: " + err);

  // 3D: a v16 starter scene with one script on the crate.
  scene3d::Scene3D s = scene3d::make_starter_scene();
  if (s.entities.empty()) return fail("3D starter scene");
  scene3d::Entity3D& crate = s.entities.back();
  crate.script = ScriptData{};
  crate.script->path = "scripts/crate.lua";
  const std::string text3 = scene3d::write(s);
  if (has(text3, "\"scripts\"")) return fail("3D: one script should still save as \"script\"");
  scene3d::Scene3D old3;
  if (!scene3d::parse(with_version(text3, 16), &old3, &err)) return fail("3D v16 parse: " + err);
  const scene3d::Entity3D* c = old3.find(crate.id);
  if (!c || script_count(*c) != 1 || script_stack(*c)[0].path != "scripts/crate.lua")
    return fail("3D v16 single script loads as a one-script stack");
  return 0;
}

// --- 4. Play: every script's hooks in Inspector order ------------------------------

const char* kLuaFirst = R"(-- first on the wagon
function on_start() log("order start Lua1 " .. self:name()) end
function on_interact(rider) log("order interact Lua1") end
)";
const char* kLuaSecond = R"(-- third on the wagon
props = { word = "howdy" }
function on_start() log("order start Lua2 " .. self:name() .. " " .. props.word) end
function on_interact(rider) log("order interact Lua2") end
)";
const char* kLuaOff = R"(function on_start() log("order start OFF") end
function on_interact(rider) log("order interact OFF") end
)";
const char* kLuaBoom = R"(function on_interact(rider)
  local saddle = nil
  return saddle.horn
end
)";
const char* kCpp = R"(#include <tombstone/script_sdk.h>
class OrderCpp : public ts::Script {
 public:
  void on_start() override { log("order start Cpp"); }
  void on_interact(ts::Entity) override { log("order interact Cpp"); }
};
TS_SCRIPT(OrderCpp, "order_cpp")
)";
const char* kCs = R"(using TombStone;
[Script("order_cs")]
public class OrderCs : Script {
    public override void OnStart() { Log("order start Cs"); }
    public override void OnInteract(ulong rider) { Log("order interact Cs"); }
}
)";

int check_play(const fs::path& proj, bool cpp_ok, bool cs_ok, std::string* note) {
  std::error_code ec;
  fs::remove_all(proj, ec);
  write_text(proj / "scripts" / "first.lua", kLuaFirst);
  write_text(proj / "scripts" / "second.lua", kLuaSecond);
  write_text(proj / "scripts" / "off.lua", kLuaOff);
  write_text(proj / "scripts" / "boom.lua", kLuaBoom);
  write_text(proj / "scripts" / "order_cpp.cpp", kCpp);
  write_text(proj / "scripts" / "order_cs.cs", kCs);
  const NewScriptResult graph = create_script_file(proj.string(), "wagon ticker", ScriptLang::Graph, "ticker");
  if (!graph.ok) return fail("graph ticker: " + graph.message);

  // Wagon: Lua1, [C++], OFF (disabled), graph, Lua2, [C#], boom.
  std::vector<ScriptData> stack;
  std::vector<std::string> expect;  // interact order
  auto push = [&](const std::string& rel, bool on = true) {
    ScriptData s;
    s.path = rel;
    s.enabled = on;
    stack.push_back(s);
  };
  push("scripts/first.lua");
  expect.push_back("Lua1");
  if (cpp_ok) {
    push("scripts/order_cpp.cpp");
    expect.push_back("Cpp");
  }
  push("scripts/off.lua", false);
  push(graph.rel);
  push("scripts/second.lua");
  stack.back().set("word", ScriptValue::of_text("partner"));
  expect.push_back("Lua2");
  if (cs_ok) {
    push("scripts/order_cs.cs");
    expect.push_back("Cs");
  }
  push("scripts/boom.lua");
  Entity2D wagon;
  wagon.id = 2;
  wagon.name = "Wagon";
  set_script_stack(wagon, stack);
  Entity2D post;  // a one-script neighbour: its first slot rides in phase 1
  post.id = 3;
  post.name = "Post";
  set_script_stack(post, {stack[0]});

  runtime::native_scripts::set_blocking_builds(true);
  struct Unblock { ~Unblock() { runtime::native_scripts::set_blocking_builds(false); } } unblock;
  runtime::World w;
  w.set_extensions(smoke_plugin_extensions());
  std::string err;
  if (!w.build({wagon, post}, proj.string(), &err)) return fail("build: " + err + dump(w.take_logs()));
  std::vector<runtime::LogEntry> logs = w.take_logs();
  for (int i = 0; i < 20; ++i) {
    w.step(runtime::InputFrame{});
    for (runtime::LogEntry& e : w.take_logs()) logs.push_back(std::move(e));
  }
  runtime::ScriptHost* host = w.scripts();
  const std::size_t enabled = stack.size() - 1;
  if (!host || host->script_count(2) != enabled || host->script_count(3) != 1)
    return fail("every enabled script attaches (" + std::to_string(enabled) + ")" + dump(logs));
  if (find_log(logs, "order start OFF") >= 0) return fail("a disabled script must not run");
  // Start: stack position by position (both objects' first scripts first).
  const int s_wagon1 = find_log(logs, "order start Lua1 Wagon"), s_post = find_log(logs, "order start Lua1 Post");
  const int s_lua2 = find_log(logs, "order start Lua2 Wagon partner");
  if (s_wagon1 < 0 || s_post < 0 || s_lua2 < 0 || s_lua2 < s_wagon1 || s_lua2 < s_post)
    return fail("on_start order / per-script props:" + dump(logs));
  if (cpp_ok) {
    const int c = find_log(logs, "order start Cpp");
    if (c < 0 || c < s_post || c > s_lua2) return fail("C++ start between Lua1 and Lua2:" + dump(logs));
  }
  if (cs_ok) {
    const int c = find_log(logs, "order start Cs");
    if (c < 0 || c < s_lua2) return fail("C# start after Lua2:" + dump(logs));
  }
  // The graph slot rides too.
  int graph_slot = -1;
  int off_slot = -1;
  for (std::size_t i = 0; i < stack.size(); ++i) {
    if (stack[i].path == graph.rel) graph_slot = static_cast<int>(i);
    if (!stack[i].enabled) off_slot = static_cast<int>(i);
  }
  if (graph_slot < 0 || !host->running(2, graph_slot)) return fail("graph slot running");
  if (off_slot < 0 || host->running(2, off_slot)) return fail("disabled slot should not run");

  // Interact: the object's scripts in Inspector order; boom stops alone.
  host->interact(2, 3);
  logs = w.take_logs();
  int last = -1;
  for (const std::string& who : expect) {
    const int at = find_log(logs, "order interact " + who);
    if (at < 0 || at < last) return fail("on_interact order (" + who + "):" + dump(logs));
    last = at;
  }
  bool named = false;
  for (const runtime::LogEntry& e : logs) {
    const std::string line = runtime::format_log(e, true);
    named = named || (e.level == runtime::LogLevel::Error && has(line, "\"Wagon\"") && has(line, "scripts/boom.lua"));
  }
  if (!named) return fail("a script error should name the object and the script:" + dump(logs));
  host->interact(2, 3);
  logs = w.take_logs();
  if (find_log(logs, "order interact Lua1") < 0 || find_log(logs, "order interact Lua2") < 0)
    return fail("the rest of the stack rides on after an error:" + dump(logs));
  *note = std::to_string(enabled) + " scripts on one object (Lua" + std::string(cpp_ok ? " + C++" : "") +
          " + graph" + (cs_ok ? " + C#" : "") + "), start by stack position, interact in Inspector order, "
          "disabled skipped, error names \"Wagon\" (scripts/boom.lua)";
  return 0;
}

// --- 5. The 3D editor ----------------------------------------------------------------

int check_editor_3d(std::string* note) {
  const fs::path dir = fs::temp_directory_path() / "ts_multi_script_3d";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  ProjectInfo info;
  info.id = "multi_script_3d";
  info.name = "Multi Script 3D";
  info.kind = ProjectKind::ThreeD;
  info.path = dir.string();
  Editor3DScreen scr(info);
  scr.on_enter();
  const scene3d::Entity3D* crate = scr.scene().find_named("Crate");
  if (!crate) return fail("3D starter crate");
  const std::uint64_t id = crate->id;
  write_text(dir / "scripts" / "a.lua", "function on_start() self:set(\"order\", (self:get(\"order\") or \"\") .. \"A\") end\n");
  write_text(dir / "scripts" / "b.lua", "function on_start() self:set(\"order\", (self:get(\"order\") or \"\") .. \"B\") end\n");
  write_text(dir / "scripts" / "c.lua", "function on_start() self:set(\"order\", (self:get(\"order\") or \"\") .. \"C\") end\n");
  scr.rescan_assets();
  if (scr.scripts().size() != 3) return fail("3D scripts list");
  std::string rel;
  if (!scr.new_script(id, "Crate Sign", ScriptLang::Graph, "sign", &rel) || !scr.attach_script(id, "scripts/b.lua") ||
      !scr.attach_script(id, "scripts/a.lua") || !scr.attach_script(id, "scripts/c.lua") ||
      scr.attach_script(id, "scripts/a.lua"))
    return fail("3D attach (New Script graph + three Lua, duplicate refused)");
  std::vector<ScriptData> stack = script_stack(*scr.scene().find(id));
  if (paths(stack) != std::vector<std::string>{rel, "scripts/b.lua", "scripts/a.lua", "scripts/c.lua"})
    return fail("3D stack order");
  stack[3].enabled = false;  // c off
  if (!scr.set_scripts(id, stack, "Disable Script")) return fail("3D disable");
  if (!scr.undo() || !script_stack(*scr.scene().find(id))[3].enabled || !scr.redo()) return fail("3D undo / redo");
  std::string err;
  if (!scr.save_scene(&err)) return fail("3D save: " + err);
  scene3d::Scene3D back;
  if (!scene3d::parse(read_text(scene3d::scene_path(info.path)), &back, &err) || !back.find(id) ||
      script_stack(*back.find(id)) != script_stack(*scr.scene().find(id)))
    return fail("3D v17 stack round trip: " + err);
  if (!scr.play_start()) return fail("3D play");
  world3d::Input3D in;
  scr.play_ticks(5, in);
  const ScriptValue* order = scr.world().script_get(id, "order");
  const std::string got = order ? order->text : "(none)";
  scr.play_stop();
  if (got != "BA") return fail("3D hooks in stack order with c disabled: " + got);
  if (scr.set_scripts(id, {}, "x") && script_count(*scr.scene().find(id)) != 0) return fail("3D clear");
  scr.on_exit();
  fs::remove_all(dir, ec);
  *note = "3D: New Script + drag-attach + disable + undo, v17 round trip, Play ran B then A (C off)";
  return 0;
}

}  // namespace

int run_scripts_multi_smoke() {
  std::string why;
  const bool cpp_ok = runtime::native_scripts::toolchain_available(&why);
  const bool cs_ok = runtime::csharp_scripts::sdk_available(&why);
  std::error_code ec;
  const fs::path tmp = fs::temp_directory_path(ec);
  std::string n1, n2, n3, n4;
  if (check_templates(tmp / "ts_multi_templates", cpp_ok, cs_ok, &n1) != 0) return 1;
  if (check_editor_2d(cs_ok, &n2) != 0) return 1;
  if (check_migration() != 0) return 1;
  if (check_play(tmp / "ts_multi_play", cpp_ok, cs_ok, &n3) != 0) return 1;
  if (check_editor_3d(&n4) != 0) return 1;
  std::cout << "[smoke] scripts multi OK (" << script_templates().size() << " templates x 4 languages, " << n1
            << "; 2D: " << n2 << "; old single-script scenes (2D v11, 3D v16) load; play: " << n3 << "; " << n4
            << ")\n";
  return 0;
}
