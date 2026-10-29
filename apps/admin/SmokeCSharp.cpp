#include "SmokeCSharp.h"

#include "ExportCli.h"
#include "SmokeGraph.h"
#include "editor/launch/GameLauncher.h"
#include "export/Exporter.h"
#include "render3d/Scene3D.h"
#include "runtime/CSharpScript.h"
#include "runtime/GraphScript.h"
#include "runtime/NativeScript.h"
#include "runtime/ScriptAbi.h"
#include "runtime/ScriptHost.h"
#include "runtime/World.h"
#include "scene/SceneJson.h"
#include "world3d/World3D.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ts::tombstone;
using namespace ts::tombstone::runtime;
namespace fs = std::filesystem;

int fail(const std::string& what) {
  std::cerr << "[smoke] C# scripts FAILED: " << what << '\n';
  return 1;
}

const char* kDoorV1 = R"(using TombStone;
[Script("door")]
public class Door : Script {
    int opened;
    public override void OnStart() {
        Log("door v1 ready, reach " + (int)Prop("reach", 24.0));
        Set("gold", 3.0);
        if (Prop("bridge", 0.0) > 0) Log("plugin says " + Plugin("frontier", "greet", "pardner"));
    }
    public override void OnTick(double dt) {
        if (Tick % 30 == 0) { opened++; Set("opened", (double)opened); }
    }
    public override void OnReload() => Log("door reloaded, opened " + opened);
    public override void OnQuestCompleted(string id) => Log("door heard quest " + id);
    public override string Save() => opened.ToString();
    public override void Load(string s) => int.TryParse(s, out opened);
}
)";

std::string v2() {
  std::string s = kDoorV1;
  s.replace(s.find("door reloaded"), 13, "door v2 reloaded");
  return s + "// v2\n";
}

void write(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

std::vector<LogEntry> ride(World& w, int ticks) {
  std::vector<LogEntry> out;
  for (int i = 0; i < ticks; ++i) {
    w.step(InputFrame{});
    for (LogEntry& e : w.take_logs()) out.push_back(std::move(e));
  }
  return out;
}

bool has(const std::vector<LogEntry>& logs, const std::string& text, LogLevel lv = LogLevel::Info) {
  for (const LogEntry& e : logs) {
    if (e.level == lv && e.text.find(text) != std::string::npos) return true;
  }
  return false;
}

std::string dump(const std::vector<LogEntry>& logs) {
  std::string d;
  for (const LogEntry& e : logs) d += "\n  " + e.file + ":" + std::to_string(e.line) + " " + e.text;
  return d.size() > 3000 ? d.substr(0, 3000) : d;
}

Entity2D scripted(std::uint64_t id, const std::string& name, const std::string& path) {
  Entity2D e;
  e.id = id;
  e.name = name;
  e.script = ScriptData{};
  e.script->path = path;
  return e;
}

// No SDK: a .cs entity is refused with the install hint; Lua beside it rides.
int check_disabled(const std::string& why) {
  std::error_code ec;
  const fs::path proj = fs::temp_directory_path(ec) / "ts_cs_disabled";
  fs::remove_all(proj, ec);
  write(proj / "scripts" / "door.cs", kDoorV1);
  write(proj / "scripts" / "bank.lua", "function on_start() log('lua still rides') end\n");
  World w;
  w.script_library().put("scripts/bank.lua", "function on_start() log('lua still rides') end\n");
  std::string err;
  w.build({scripted(2, "Door", "scripts/door.cs"), scripted(3, "Bank", "scripts/bank.lua")}, proj.string(), &err);
  std::vector<LogEntry> logs = w.take_logs();
  for (LogEntry& e : ride(w, 3)) logs.push_back(std::move(e));
  bool hinted = false;
  for (const LogEntry& e : logs) {
    hinted = hinted || (e.level == LogLevel::Warn && e.text.find("C# scripts disabled") != std::string::npos &&
                        e.text.find(".NET 8 SDK") != std::string::npos);
  }
  if (!hinted) return fail("without the SDK the Telegraph should say how to enable C#:" + dump(logs));
  if (!has(logs, "lua still rides")) return fail("Lua beside a disabled C# script should ride:" + dump(logs));
  if (w.scripts()->running(2)) return fail("a disabled C# script must not run");
  (void)why;
  return 0;
}

int check_export() {
  std::error_code ec;
  std::string err;
  const std::string sample = ts::tombstone::export_sample_dir("sample-2d-platformer", &err);
  if (sample.empty()) return fail(err);
  const fs::path proj = fs::temp_directory_path(ec) / "ts_cs_export";
  fs::remove_all(proj, ec);
  fs::copy(sample, proj, fs::copy_options::recursive, ec);
  write(proj / "scripts" / "door.cs", kDoorV1);
  scene_json::SceneDoc doc;
  if (!scene_json::load_file((proj / "scene.json").string(), &doc, &err)) return fail("scene: " + err);
  Entity2D door;
  for (const Entity2D& e : doc.entities) door.id = std::max(door.id, e.id);
  ++door.id;
  door.name = "Door";
  door.x = -400;
  door.script = ScriptData{};
  door.script->path = "scripts/door.cs";
  doc.entities.push_back(door);
  if (!scene_json::save_file((proj / "scene.json").string(), doc, &err)) return fail("scene save: " + err);
  exporter::ExportOptions opt;
  opt.project_dir = proj.string();
  opt.out_dir = (fs::temp_directory_path(ec) / "ts_cs_export_out").string();
  opt.runtime_exe = editor::launcher::find_game_executable();
  if (opt.runtime_exe.empty()) return fail("ts_game not built");
  exporter::ExportResult res;
  if (!exporter::export_project(opt, &res)) return fail("export: " + res.text());
  std::string out;
  const int rc = run_command("\"" + res.exe_path + "\" --smoke", &out);
  if (rc != 0 || out.find("door v1 ready") == std::string::npos) {
    return fail("exported game with a C# script (rc " + std::to_string(rc) + "):\n" +
                (out.size() > 2500 ? out.substr(out.size() - 2500) : out));
  }
  return 0;
}

// Lua + graph + C# (+ C++ when a compiler is here) in one world, sharing
// entity state, ticking in the fixed language order.
int check_mixed(const fs::path& proj, bool with_cpp) {
  write(proj / "scripts" / "bank.lua", "function on_start() self:set('gold', 5) log('lua start') end\n");
  write(proj / "scripts" / "teller.cs", R"(using TombStone;
public class Teller : Script {
    public override void OnStart() => Log("cs sees gold " + Get("gold", -1, Find("Bank")));
}
)");
  if (with_cpp) {
    write(proj / "scripts" / "clerk.cpp", R"(#include <tombstone/script_sdk.h>
struct Clerk : ts::Script {
  void on_start() override { log("cpp sees gold " + std::to_string(static_cast<int>(get("gold", -1, find("Bank"))))); }
};
TS_SCRIPT(Clerk, "clerk")
)");
  }
  World w;
  w.script_library().put("scripts/bank.lua", "function on_start() self:set('gold', 5) log('lua start') end\n");
  std::vector<Entity2D> ents = {scripted(2, "Bank", "scripts/bank.lua"), scripted(4, "Teller", "scripts/teller.cs")};
  if (with_cpp) ents.push_back(scripted(5, "Clerk", "scripts/clerk.cpp"));
  std::string err;
  if (!w.build(ents, proj.string(), &err)) return fail("mixed build: " + err + dump(w.take_logs()));
  std::vector<LogEntry> logs = w.take_logs();
  for (LogEntry& e : ride(w, 2)) logs.push_back(std::move(e));
  if (!has(logs, "lua start") || !has(logs, "cs sees gold 5")) return fail("mixed Lua + C#:" + dump(logs));
  if (with_cpp && !has(logs, "cpp sees gold 5")) return fail("mixed + C++:" + dump(logs));
  std::error_code ec;
  fs::remove(proj / "scripts" / "clerk.cpp", ec);
  return 0;
}

#ifndef TS_REPO_DIR
#define TS_REPO_DIR "."
#endif

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string dump3(const world3d::World3D& w) {
  std::string d;
  for (const LogEntry& e : w.log()) d += "\n  " + e.file + ":" + std::to_string(e.line) + " " + e.text;
  return d.size() > 3000 ? d.substr(0, 3000) : d;
}

// The developer's character_movement.cs, unchanged, plus the 2D coin.
// No Player component and no Rider plugin: the script walks the body.
int check_character_movement() {
  const fs::path sample =
      fs::path(TS_REPO_DIR) / "samples" / "scripting" / "fps-cs" / "scripts" / "character_movement.cs";
  const std::string text = read_text(sample);
  if (text.find("public override void OnUpdate(float deltaTime)") == std::string::npos ||
      text.find("Input.GetAxis(\"Horizontal\")") == std::string::npos ||
      text.find("mainCam.SetLocalRotation") == std::string::npos) {
    return fail("samples/scripting/fps-cs/scripts/character_movement.cs is missing or was edited");
  }
  const std::vector<ScriptProp> declared = describe_compiled_props(text, true);
  bool speed = false, grav = false;
  for (const ScriptProp& p : declared) {
    if (p.name == "moveSpeed" && std::fabs(p.value.number - 6.0) < 1e-4) speed = true;
    if (p.name == "gravity" && std::fabs(p.value.number - (-9.81)) < 1e-3) grav = true;
  }
  if (!speed || !grav) return fail("Inspector props should include moveSpeed 6 and gravity -9.81");

  std::error_code ec;
  const fs::path proj = fs::temp_directory_path(ec) / "ts_cs_fps";
  fs::remove_all(proj, ec);
  write(proj / "scripts" / "character_movement.cs", text);
  write(proj / "scripts" / "coin.cs",
        read_text(fs::path(TS_REPO_DIR) / "samples" / "scripting" / "2d" / "scripts" / "coin.cs"));

  scene3d::Scene3D scene;
  scene.version = scene3d::kScene3DVersion;
  scene3d::Entity3D body;
  body.id = 1;
  body.name = "Wanderer";
  body.transform.position = {0.0f, 0.05f, 0.0f};
  scene3d::Collider3D cap;
  cap.shape = scene3d::Collider3D::Shape::Capsule;
  cap.size = {0.4f, 1.8f, 0.4f};
  cap.offset = {0.0f, 0.9f, 0.0f};
  body.collider = cap;
  body.script = ScriptData{};
  body.script->path = "scripts/character_movement.cs";
  body.script->set("moveSpeed", ScriptValue::of_number(12));
  scene3d::Entity3D cam;
  cam.id = 2;
  cam.name = "MainCamera";
  cam.parent = 1;
  cam.transform.position = {0.0f, 1.6f, 0.0f};
  scene3d::PlayCamera3D pc;
  pc.rig = scene3d::PlayCamera3D::Rig::Fixed;
  pc.fov = 75.0f;
  pc.active = true;
  pc.priority = 1;
  cam.camera = pc;
  scene3d::Entity3D ground;
  ground.id = 3;
  ground.name = "Ground";
  ground.transform.position = {0.0f, -0.25f, 0.0f};
  ground.transform.scale = {40.0f, 0.5f, 40.0f};
  scene3d::Collider3D box;
  box.shape = scene3d::Collider3D::Shape::Box;
  box.size = {1, 1, 1};
  ground.collider = box;
  scene.entities = {body, cam, ground};

  world3d::World3D world;
  std::string err;
  if (!world.start(scene, proj.string(), &err)) return fail("fps start: " + err + dump3(world));
  if (world.player_id() != 0) return fail("the C# sample must run with no Rider / Player component");
  world3d::Input3D walk;
  walk.move_forward = 1.0f;
  for (int i = 0; i < 30; ++i) world.step(walk);
  const scene3d::Entity3D* walked = world.find(1);
  if (!walked || !(walked->transform.position.z < -4.5f)) {
    const float z = walked ? walked->transform.position.z : 0;
    return fail("C# controller did not walk with Prop moveSpeed 12 (z " + std::to_string(z) + ")" + dump3(world));
  }
  bool spawned = false, toasted = false;
  for (const LogEntry& e : world.log()) {
    spawned = spawned || e.text.find("Player spawned at initial position:") != std::string::npos;
    toasted = toasted || e.text.find("Character Movement Initialized") != std::string::npos;
  }
  if (!spawned || !toasted) return fail("OnStart Log / Toast:" + dump3(world));

  world3d::Input3D look;
  look.mouse_dx = 10.0f;
  world.step(look);
  const render3d::Vec3 fwd = world.find(1)->transform.rotation.rotate({0, 0, -1});
  if (fwd.x <= 0.05f)
    return fail("mouse right should yaw the body right (forward.x " + std::to_string(fwd.x) + ")");
  look = {};
  look.mouse_dy = -10.0f;
  world.step(look);
  const render3d::Vec3 eyes = world.find(2)->transform.rotation.rotate({0, 0, -1});
  if (eyes.y <= 0.05f)
    return fail("mouse up should pitch MainCamera up (forward.y " + std::to_string(eyes.y) + ")");

  std::string edited = text;
  const std::size_t brace = edited.rfind('}');
  if (brace == std::string::npos) return fail("sample has no class close");
  edited.insert(brace, "\n    public override void OnReload() { Log(\"character reloaded y=\" + velocity.Y); }\n");
  write(proj / "scripts" / "character_movement.cs", edited);
  if (world.reload_scripts() != 1) return fail("C# hot reload of the controller" + dump3(world));
  bool kept = false, wiped = false;
  for (const LogEntry& e : world.log()) {
    if (e.text.find("character reloaded") == std::string::npos) continue;
    kept = true;
    wiped = wiped || e.text.find("y=0") != std::string::npos;
  }
  if (!kept || wiped) return fail("hot reload should keep velocity" + dump3(world));

  World flat;
  Entity2D coin_e = scripted(2, "Coin", "scripts/coin.cs");
  coin_e.script->set("worth", ScriptValue::of_number(5));
  if (!flat.build({coin_e}, proj.string(), &err)) return fail("2D coin build: " + err + dump(flat.take_logs()));
  std::vector<LogEntry> logs = flat.take_logs();
  for (LogEntry& e : ride(flat, 2)) logs.push_back(std::move(e));
  if (!has(logs, "coin worth 5")) return fail("2D coin OnStart / Prop:" + dump(logs));
  world.stop();
  return 0;
}

// A prebuilt assembly (what exports ship) needs only the .NET 8+ runtime:
// TS_SMOKE_CS_PREBUILT=<folder with ProjectScripts.dll + TombStone.Scripting.*>
// rides it in a project with no SDK on the machine.
int check_prebuilt(const fs::path& from, std::string* said) {
  std::error_code ec;
  const fs::path proj = fs::temp_directory_path(ec) / "ts_cs_prebuilt";
  fs::remove_all(proj, ec);
  fs::create_directories(proj / "dotnet", ec);
  for (const auto& e : fs::directory_iterator(from, ec)) {
    fs::copy_file(e.path(), proj / "dotnet" / e.path().filename(), fs::copy_options::overwrite_existing, ec);
  }
  World w;
  Entity2D door = scripted(2, "Door", "scripts/door.cs");
  door.script->set("reach", ScriptValue::of_number(40));
  std::string err;
  w.build({door}, proj.string(), &err);
  std::vector<LogEntry> logs = w.take_logs();
  for (LogEntry& e : ride(w, 31)) logs.push_back(std::move(e));
  if (!has(logs, "door v1 ready, reach 40")) return fail("prebuilt assembly (" + from.string() + "):" + dump(logs));
  const ScriptValue* opened = w.script_get(2, "opened");
  if (!opened || opened->number != 1) return fail("prebuilt OnTick" + dump(logs));
  *said = "prebuilt assembly hosted on the .NET " + csharp_scripts::dotnet_info().runtime + " runtime";
  return 0;
}

}  // namespace

int run_csharp_smoke() {
  const bool require = std::getenv("TS_SMOKE_REQUIRE_CS") != nullptr;
  std::string why;
  if (!csharp_scripts::sdk_available(&why)) {
    if (int r = check_disabled(why)) return r;
    if (const char* pre = std::getenv("TS_SMOKE_CS_PREBUILT"); pre && *pre) {
      std::string said;
      if (int r = check_prebuilt(pre, &said)) return r;
      std::cout << "[smoke] C# scripts: no SDK (" << why << "), " << said << " OK\n";
      return 0;
    }
    std::cout << "[smoke] C# scripts SKIPPED (" << why << "; graceful disable checked)\n";
    return require ? 1 : 0;
  }
  native_scripts::set_blocking_builds(true);
  struct Unblock { ~Unblock() { native_scripts::set_blocking_builds(false); } } unblock;
  std::error_code ec;
  const fs::path proj = fs::temp_directory_path(ec) / "ts_cs_smoke";
  fs::remove_all(proj, ec);
  write(proj / "scripts" / "door.cs", kDoorV1);
  World w;
  w.set_extensions(smoke_plugin_extensions());
  Entity2D door = scripted(2, "Door", "scripts/door.cs");
  door.script->set("reach", ScriptValue::of_number(40));
  door.script->set("bridge", ScriptValue::of_number(1));
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  if (!w.build({door}, proj.string(), &err)) return fail("build: " + err + dump(w.take_logs()));
  const double build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::vector<LogEntry> logs = w.take_logs();
  for (LogEntry& e : ride(w, 60)) logs.push_back(std::move(e));
  if (!has(logs, "door v1 ready, reach 40")) return fail("OnStart / props:" + dump(logs));
  if (!has(logs, "plugin says howdy pardner")) return fail("plugin call bridge from C#:" + dump(logs));
  const ScriptValue* opened = w.script_get(2, "opened");
  if (!opened || opened->number != 2) return fail("OnTick + state (opened)" + dump(logs));
  w.script_broadcast("on_quest_completed", "bell", 0);
  if (!has(w.take_logs(), "door heard quest bell")) return fail("story hook (OnQuestCompleted) in C#");

  // Hot reload: new assembly in a new load context, state through Save/Load.
  write(proj / "scripts" / "door.cs", v2());
  if (w.reload_scripts() != 1) return fail("reload should rebuild once" + dump(w.take_logs()));
  logs = w.take_logs();
  for (LogEntry& e : ride(w, 1)) logs.push_back(std::move(e));
  if (!has(logs, "door v2 reloaded, opened 2")) return fail("hot reload keeps state:" + dump(logs));
  const int live = csharp_scripts::live_contexts();
  if (live != 1) return fail("the old AssemblyLoadContext should unload (live " + std::to_string(live) + ")");

  // A broken save: CS error with file:line, v2 keeps riding.
  write(proj / "scripts" / "door.cs", v2() + "this is not C#\n");
  w.reload_scripts();
  logs = w.take_logs();
  bool located = false;
  for (const LogEntry& e : logs) {
    located = located || (e.level == LogLevel::Error && e.file == "scripts/door.cs" && e.line > 0);
  }
  if (!located || !has(logs, "previous module keeps riding", LogLevel::Error)) {
    return fail("compile error should reach the Telegraph with file:line:" + dump(logs));
  }
  if (!w.scripts()->running(2)) return fail("old assembly should keep running");

  // Background rebuild swaps in while the world ticks.
  native_scripts::set_blocking_builds(false);
  write(proj / "scripts" / "door.cs", v2() + "// v3\n");
  w.reload_scripts();
  int bg_ticks = 0;
  bool swapped = false;
  const auto bg0 = std::chrono::steady_clock::now();
  while (!swapped && std::chrono::steady_clock::now() - bg0 < std::chrono::minutes(5)) {
    for (LogEntry& e : ride(w, 1)) swapped = swapped || e.text.find("Reloaded C# scripts") != std::string::npos;
    swapped = w.reload_scripts() == 1 || swapped;
    ++bg_ticks;
    if (!swapped) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!swapped) return fail("background rebuild never swapped in");
  native_scripts::set_blocking_builds(true);

  std::string cwhy;
  const bool cpp = native_scripts::toolchain_available(&cwhy);
  if (int r = check_mixed(proj, cpp)) return r;
  if (int r = check_character_movement()) return r;
  if (int r = check_export()) return r;
  std::cout << "[smoke] C# scripts OK (.NET SDK " << csharp_scripts::dotnet_info().sdk << ", assembly built in "
            << static_cast<int>(build_s) << " s, hooks + props + state + plugin bridge, hot reload kept state "
            << "and unloaded the old AssemblyLoadContext, compile error at scripts/door.cs:line kept the old "
            << "assembly, background rebuild swapped in after " << bg_ticks << " live ticks, mixed Lua + C#"
            << (cpp ? " + C++" : "") << ", character_movement.cs walked and hot-reloaded with no Rider, "
            << "2D coin Prop round-trip, exported game ran the shipped assembly)\n";
  return 0;
}
