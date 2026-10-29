#include "SmokeNative.h"
#include "SmokeGraph.h"

#include "ExportCli.h"
#include "editor/launch/GameLauncher.h"
#include "export/Exporter.h"
#include "render3d/Scene3D.h"
#include "runtime/NativeScript.h"
#include "runtime/ScriptAbi.h"
#include "scene/SceneJson.h"
#include "runtime/World.h"
#include "world3d/World3D.h"

#include <algorithm>
#include <cctype>
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

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

using namespace ts::tombstone;
using namespace ts::tombstone::runtime;
namespace fs = std::filesystem;

int fail(const std::string& what) {
  std::cerr << "[smoke] C++ scripts FAILED: " << what << '\n';
  return 1;
}

const char* kDoorV1 = R"(#include <tombstone/script_sdk.h>
struct Door : ts::Script {
  int opened = 0;
  void on_start() override {
    log("door v1 ready, reach " + std::to_string(static_cast<int>(prop("reach", 24.0))));
    set("gold", 3.0);
    if (prop("bridge", 0.0) > 0) log("plugin says " + plugin("frontier", "greet", {ts::arg("pardner")}).text);
  }
  void on_tick(double) override {
    if (tick() % 30 == 0) { ++opened; set("opened", static_cast<double>(opened)); }
  }
  void on_reload() override { log("door reloaded, opened " + std::to_string(opened)); }
  void on_quest_completed(const std::string& id) override { log("door heard quest " + id); }
  std::string save() const override { return std::to_string(opened); }
  void load(const std::string& s) override { opened = std::atoi(s.c_str()); }
};
TS_SCRIPT(Door, "door")
)";

std::string v2() {
  std::string s = kDoorV1;
  s.replace(s.find("door reloaded"), 13, "door v2 reloaded");
  return s + "// v2: longer file\n";
}

void write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
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

// Ship it: a 2D sample with a C++ door, exported (module cooked into the
// pak, sources left home) and ridden by the exported ts_game --smoke.
int check_export() {
  std::error_code ec;
  std::string err;
  const std::string sample = ts::tombstone::export_sample_dir("sample-2d-platformer", &err);
  if (sample.empty()) return fail(err);
  const fs::path proj = fs::temp_directory_path(ec) / "ts_native_export";
  fs::remove_all(proj, ec);
  fs::copy(sample, proj, fs::copy_options::recursive, ec);
  write(proj / "scripts" / "door.cpp", kDoorV1);
  scene_json::SceneDoc doc;
  if (!scene_json::load_file((proj / "scene.json").string(), &doc, &err)) return fail("scene: " + err);
  Entity2D door;
  for (const Entity2D& e : doc.entities) door.id = std::max(door.id, e.id);
  ++door.id;
  door.name = "Door";
  door.x = -400;
  door.script = ScriptData{};
  door.script->path = "scripts/door.cpp";
  doc.entities.push_back(door);
  if (!scene_json::save_file((proj / "scene.json").string(), doc, &err)) return fail("scene save: " + err);
  exporter::ExportOptions opt;
  opt.project_dir = proj.string();
  opt.out_dir = (fs::temp_directory_path(ec) / "ts_native_export_out").string();
  opt.runtime_exe = editor::launcher::find_game_executable();
  if (opt.runtime_exe.empty()) return fail("ts_game not built");
  exporter::ExportResult res;
  if (!exporter::export_project(opt, &res)) return fail("export: " + res.text());
  std::string out;
  const int rc = run_command("\"" + res.exe_path + "\" --smoke", &out);
  if (rc != 0 || out.find("door v1 ready") == std::string::npos) {
    return fail("exported game with a C++ script (rc " + std::to_string(rc) + "):\n" +
                (out.size() > 2500 ? out.substr(out.size() - 2500) : out));
  }
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
  for (const LogEntry& e : w.log()) d += "\n  " + e.text;
  return d.size() > 2500 ? d.substr(0, 2500) : d;
}

int check_character_movement() {
  const fs::path sample =
      fs::path(TS_REPO_DIR) / "samples" / "scripting" / "fps-cpp" / "scripts" / "character_movement.cpp";
  const std::string text = read_text(sample);
  if (text.find("void OnUpdate") == std::string::npos || text.find("GetAxis(\"Horizontal\")") == std::string::npos)
    return fail("samples/scripting/fps-cpp/scripts/character_movement.cpp is missing");
  std::error_code ec;
  const fs::path proj = fs::temp_directory_path(ec) / "ts_cpp_fps";
  fs::remove_all(proj, ec);
  write(proj / "scripts" / "character_movement.cpp", text);
  const std::string coin =
      read_text(fs::path(TS_REPO_DIR) / "samples" / "scripting" / "2d" / "scripts" / "coin.cpp");
  write(proj / "scripts" / "coin.cpp", coin);

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
  body.script->path = "scripts/character_movement.cpp";
  body.script->set("moveSpeed", ScriptValue::of_number(12));
  scene3d::Entity3D cam;
  cam.id = 2;
  cam.name = "MainCamera";
  cam.parent = 1;
  cam.transform.position = {0.0f, 1.6f, 0.0f};
  scene3d::PlayCamera3D pc;
  pc.rig = scene3d::PlayCamera3D::Rig::Fixed;
  pc.active = true;
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
  if (world.player_id() != 0) return fail("the C++ sample must run with no Rider / Player component");
  world3d::Input3D walk;
  walk.move_forward = 1.0f;
  for (int i = 0; i < 30; ++i) world.step(walk);
  const scene3d::Entity3D* walked = world.find(1);
  if (!walked || !(walked->transform.position.z < -4.5f)) {
    const float z = walked ? walked->transform.position.z : 0;
    return fail("C++ controller did not walk with Prop moveSpeed 12 (z " + std::to_string(z) + ")" + dump3(world));
  }
  world3d::Input3D look;
  look.mouse_dx = 10.0f;
  world.step(look);
  if (world.find(1)->transform.rotation.rotate({0, 0, -1}).x <= 0.05f)
    return fail("C++ mouse right should yaw right");
  look = {};
  look.mouse_dy = -10.0f;
  world.step(look);
  if (world.find(2)->transform.rotation.rotate({0, 0, -1}).y <= 0.05f)
    return fail("C++ mouse up should pitch MainCamera up");

  write(proj / "scripts" / "character_movement.cpp", text + "\n// reload\n");
  if (world.reload_scripts() != 1) return fail("C++ hot reload of the controller" + dump3(world));
  bool kept = false, wiped = false;
  for (const LogEntry& e : world.log()) {
    if (e.text.find("character reloaded") == std::string::npos) continue;
    kept = true;
    wiped = wiped || e.text.find("y=0") != std::string::npos;
  }
  if (!kept || wiped) return fail("C++ hot reload should keep velocity" + dump3(world));

  World flat;
  Entity2D coin_e;
  coin_e.id = 2;
  coin_e.name = "Coin";
  coin_e.script = ScriptData{};
  coin_e.script->path = "scripts/coin.cpp";
  coin_e.script->set("worth", ScriptValue::of_number(5));
  if (!flat.build({coin_e}, proj.string(), &err)) return fail("2D coin build: " + err);
  std::vector<LogEntry> logs = flat.take_logs();
  for (LogEntry& e : ride(flat, 2)) logs.push_back(std::move(e));
  if (!has(logs, "coin worth 5")) return fail("2D C++ coin:" + dump(logs));
  // Leave the fps module out of the door project's later export.
  fs::remove(proj / "scripts" / "coin.cpp", ec);
  world.stop();
  return 0;
}

}  // namespace

// Compiler output -> Telegraph file:line, whatever spelling the compiler
// uses for the project folder (CI runners hand out 8.3 short temp paths
// while MSVC prints long ones). Runs without a toolchain.
int check_build_output_paths(std::string* note) {
  std::error_code ec;
  const fs::path proj = fs::temp_directory_path(ec) / "ts_native_paths_LongFolderName";
  fs::remove_all(proj, ec);
  write(proj / "scripts" / "door.cpp", "// door\n");
  struct Clean { fs::path p; ~Clean() { std::error_code e; fs::remove_all(p, e); } } clean{proj};
  const std::string root = proj.string();
  auto located = [](const std::vector<LogEntry>& logs) {
    return logs.size() == 1 && logs[0].file == "scripts/door.cpp" && logs[0].line == 19 &&
           logs[0].level == LogLevel::Error;
  };
  struct Case { std::string what, output, project; };
  std::vector<Case> cases = {
      {"gcc", (proj / "scripts" / "door.cpp").generic_string() + ":19:5: error: expected ';'", root},
      {"msvc", (proj / "scripts" / "door.cpp").string() + "(19): error C2143: syntax error: missing ';'", root},
      {"dot-dot", (proj / "scripts" / ".." / "scripts" / "door.cpp").generic_string() + ":19: error: x", root},
      {"trailing slash root", (proj / "scripts" / "door.cpp").generic_string() + ":19: error: x",
       proj.generic_string() + "/"},
  };
  int extra = 0;
#if defined(_WIN32)
  {
    std::string upper = (proj / "scripts" / "door.cpp").string();
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    cases.push_back({"upper case", upper + "(19,5): error C2143: x", root});
    std::string lower = (proj / "scripts" / "door.cpp").string();
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    cases.push_back({"lower case (MSVC /FC)", lower + "(19): error C2143: x", root});
    const std::wstring wl = proj.wstring();
    std::wstring ws(MAX_PATH * 4, L'\0');
    const DWORD n = GetShortPathNameW(wl.c_str(), ws.data(), static_cast<DWORD>(ws.size()));
    if (n > 0 && n < ws.size()) {
      ws.resize(n);
      const fs::path short_proj(ws);
      if (short_proj != proj) {
        // Short project folder, long compiler path (the CI case) and back.
        cases.push_back({"8.3 project dir", (proj / "scripts" / "door.cpp").string() + "(19): error C2143: x",
                         short_proj.string()});
        cases.push_back({"8.3 compiler path", (short_proj / "scripts" / "door.cpp").string() + "(19): error C2143: x",
                         root});
        ++extra;
      }
    }
  }
#endif
  for (const Case& c : cases) {
    const std::vector<LogEntry> logs = parse_build_output(c.output + "\n", c.project);
    if (!located(logs)) {
      std::string got = logs.empty() ? "nothing" : logs[0].file + ":" + std::to_string(logs[0].line);
      return fail("build output path (" + c.what + ") should map to scripts/door.cpp:19, got " + got +
                  " from '" + c.output + "' in '" + c.project + "'");
    }
  }
  if (project_relative_path("/elsewhere/door.cpp", root) != "/elsewhere/door.cpp") {
    return fail("a file outside the project should keep its path");
  }
  *note = std::to_string(cases.size()) + " path spellings" + (extra ? " incl. 8.3 short names" : "");
  return 0;
}

int run_native_smoke() {
  const bool require = std::getenv("TS_SMOKE_REQUIRE_CPP") != nullptr;
  std::string why;
  std::string path_note;
  if (check_build_output_paths(&path_note) != 0) return 1;
  if (!native_scripts::toolchain_available(&why)) {
    std::cout << "[smoke] C++ scripts SKIPPED (" << why << ")\n";
    return require ? 1 : 0;
  }
  std::error_code ec;
  const fs::path proj = fs::temp_directory_path(ec) / "ts_native_smoke";
  fs::remove_all(proj, ec);
  write(proj / "scripts" / "door.cpp", kDoorV1);
  native_scripts::set_blocking_builds(true);
  struct Unblock { ~Unblock() { native_scripts::set_blocking_builds(false); } } unblock;
  World w;
  w.set_extensions(smoke_plugin_extensions());
  Entity2D door;
  door.id = 2;
  door.name = "Door";
  door.script = ScriptData{};
  door.script->path = "scripts/door.cpp";
  door.script->set("reach", ScriptValue::of_number(40));
  door.script->set("bridge", ScriptValue::of_number(1));
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  if (!w.build({door}, proj.string(), &err)) {
    const std::vector<LogEntry> logs = w.take_logs();
    if (has(logs, "did not compile", LogLevel::Error) || has(logs, "could not configure", LogLevel::Error)) {
      std::cout << "[smoke] C++ scripts SKIPPED (no working C++ compiler for the module:" << dump(logs) << ")\n";
      return require ? 1 : 0;
    }
    return fail("build: " + err + dump(logs));
  }
  const double build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::vector<LogEntry> logs = w.take_logs();
  for (LogEntry& e : ride(w, 60)) logs.push_back(std::move(e));
  if (!has(logs, "door v1 ready, reach 40")) return fail("on_start / props:" + dump(logs));
  if (!has(logs, "plugin says howdy pardner")) return fail("plugin call bridge from C++:" + dump(logs));
  const ScriptValue* opened = w.script_get(2, "opened");
  if (!opened || opened->number != 2) return fail("on_tick + state set (opened)" + dump(logs));
  w.script_broadcast("on_quest_completed", "bell", 0);
  if (!has(w.take_logs(), "door heard quest bell")) return fail("story hook (on_quest_completed) in C++");

  // Hot reload: new code, member state kept through save() / load().
  write(proj / "scripts" / "door.cpp", v2());
  if (w.reload_scripts() != 1) return fail("reload_changed should rebuild once" + dump(w.take_logs()));
  logs = w.take_logs();
  for (LogEntry& e : ride(w, 1)) logs.push_back(std::move(e));
  if (!has(logs, "door v2 reloaded, opened 2")) return fail("hot reload keeps state:" + dump(logs));

  // A broken save: errors with file:line, the v2 module keeps riding.
  write(proj / "scripts" / "door.cpp", v2() + "this is not C++;\n");
  w.reload_scripts();
  logs = w.take_logs();
  bool located = false;
  for (const LogEntry& e : logs) {
    located = located || (e.level == LogLevel::Error && e.file == "scripts/door.cpp" && e.line > 0);
  }
  if (!located || !has(logs, "previous module keeps riding", LogLevel::Error)) {
    return fail("compile error should reach the Telegraph with file:line:" + dump(logs));
  }
  if (!w.scripts()->running(2)) return fail("old module should keep running");
  for (LogEntry& e : ride(w, 30)) (void)e;
  const ScriptValue* after = w.script_get(2, "opened");
  if (!after || after->number < 3) return fail("still ticking after a failed reload");
  // Background build: the tick keeps running while the module compiles,
  // the new code swaps in when ready.
  native_scripts::set_blocking_builds(false);
  write(proj / "scripts" / "door.cpp", v2() + "// v3\n");
  if (w.reload_scripts() != 0) return fail("a background rebuild should not swap at once");
  int bg_ticks = 0;
  bool swapped = false;
  const auto bg0 = std::chrono::steady_clock::now();
  while (!swapped && std::chrono::steady_clock::now() - bg0 < std::chrono::minutes(5)) {
    for (LogEntry& e : ride(w, 1)) swapped = swapped || e.text.find("Reloaded C++ scripts") != std::string::npos;
    swapped = w.reload_scripts() == 1 || swapped;
    ++bg_ticks;
    if (!swapped) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!swapped) return fail("background rebuild never swapped in");
  if (bg_ticks < 2) return fail("the game should tick while the module builds");
  if (!w.scripts()->running(2)) return fail("running after the background swap");
  native_scripts::set_blocking_builds(true);
  if (int r = check_character_movement()) return r;
  if (int r = check_export()) return r;
  std::cout << "[smoke] C++ scripts OK (module built in " << static_cast<int>(build_s)
            << " s, hooks + props + state + plugin bridge, hot reload kept state, compile error at "
               "scripts/door.cpp:line kept the old module, background rebuild swapped in after " << bg_ticks
            << " live ticks, character_movement.cpp walked and hot-reloaded with no Rider, "
            << "exported game ran the module from its pak, compiler paths: " << path_note << ")\n";
  return 0;
}
