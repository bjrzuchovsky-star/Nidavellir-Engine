#include "SmokeNative.h"

#include "ExportCli.h"
#include "editor/launch/GameLauncher.h"
#include "export/Exporter.h"
#include "runtime/NativeScript.h"
#include "runtime/ScriptAbi.h"
#include "scene/SceneJson.h"
#include "runtime/World.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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
  }
  void on_tick(double) override {
    if (tick() % 30 == 0) { ++opened; set("opened", static_cast<double>(opened)); }
  }
  void on_reload() override { log("door reloaded, opened " + std::to_string(opened)); }
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

}  // namespace

int run_native_smoke() {
  const bool require = std::getenv("TS_SMOKE_REQUIRE_CPP") != nullptr;
  std::string why;
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
  Entity2D door;
  door.id = 2;
  door.name = "Door";
  door.script = ScriptData{};
  door.script->path = "scripts/door.cpp";
  door.script->set("reach", ScriptValue::of_number(40));
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
  const ScriptValue* opened = w.script_get(2, "opened");
  if (!opened || opened->number != 2) return fail("on_tick + state set (opened)" + dump(logs));

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
  if (int r = check_export()) return r;
  std::cout << "[smoke] C++ scripts OK (module built in " << static_cast<int>(build_s)
            << " s, hooks + props + state, hot reload kept state, compile error at "
               "scripts/door.cpp:line kept the old module, background rebuild swapped in after " << bg_ticks
            << " live ticks, exported game ran the module from its pak)\n";
  return 0;
}
