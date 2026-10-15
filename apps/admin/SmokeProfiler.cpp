// --smoke-profiler: the Assay Office core, captures, console commands, the
// in-game debug tools, worlds feeding scopes, the optional plugin and the
// editor panels during Play.

#include "SmokeProfiler.h"

#include "debugtools/DebugTools.h"
#include "debugtools/ProfilerWorld3D.h"
#include "editor/ProjectInfo.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/plugins/EditorPlugins.h"
#include "editor/plugins/PluginPanelHost.h"
#include "editor/plugins/ProfilerPanels.h"
#include "plugin/Catalog.h"
#include "plugin/PluginManager.h"
#include "plugins/Builtins.h"
#include "plugins/profiler/ProfilerPlugin.h"
#include "profiler/Console.h"
#include "profiler/Profiler.h"
#include "render3d/Scene3D.h"
#include "runtime/Input.h"
#include "runtime/World.h"
#include "scene/SceneData.h"
#include "ui/Ui.h"
#include "world3d/World3D.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace ts::tombstone;
using profiler::Profiler;

namespace {

int fail(const std::string& m) {
  std::cerr << "[smoke] profiler FAIL: " << m << "\n";
  Profiler::get().set_enabled(false);
  return 1;
}

void spin_ms(double ms) {
  const double t0 = profiler::now_ms();
  while (profiler::now_ms() - t0 < ms) {
  }
}

bool has_zone(const std::vector<profiler::FrameRecord>& frames, const std::string& name) {
  for (const auto& f : frames)
    for (const auto& z : f.zones)
      if (z.name == name) return true;
  return false;
}

int core_checks() {
  Profiler& p = Profiler::get();
  p.clear();
  p.set_enabled(false);
  p.begin_frame();
  { TS_PROFILE_SCOPE("off"); }
  p.end_frame();
  if (p.frame_count() != 0) return fail("disabled profiler recorded a frame");
  p.set_enabled(true);
  p.set_history(8);
  for (int i = 0; i < 12; ++i) {
    p.begin_frame();
    {
      TS_PROFILE_SCOPE("outer");
      spin_ms(0.3);
      {
        TS_PROFILE_SCOPE("inner");
        spin_ms(0.4);
      }
      { TS_PROFILE_SCOPE("inner"); }
    }
    p.counter("render.draws", 7);
    p.add("world.ticks", 1);
    p.add("world.ticks", 1);
    p.gpu("opaque", 1.5);
    p.end_frame();
  }
  if (p.frame_count() != 8) return fail("ring should keep 8 frames, has " + std::to_string(p.frame_count()));
  profiler::FrameRecord f;
  if (!p.last(&f)) return fail("no last frame");
  if (f.zones.size() != 3) return fail("expected 3 zones per frame");
  const profiler::Zone& outer = f.zones[0];
  const profiler::Zone& inner = f.zones[1];
  if (outer.name != "outer" || outer.depth != 0 || inner.name != "inner" || inner.depth != 1)
    return fail("zone nesting");
  if (inner.start_ms < outer.start_ms || inner.start_ms + inner.dur_ms > outer.start_ms + outer.dur_ms + 1e-6)
    return fail("inner zone should sit inside outer");
  if (f.counters["world.ticks"] != 2 || f.counters["render.draws"] != 7) return fail("counters");
  if (f.gpu_ms["opaque"] != 1.5) return fail("gpu timing");
  const auto stats = p.zone_stats();
  if (stats.empty() || stats[0].name != "outer") return fail("outer should be the slowest scope");
  for (const auto& s : stats) {
    if (s.name == "inner" && s.calls != 16) return fail("inner calls over 8 frames should be 16");
    if (s.name == "outer" && s.self_ms > s.total_ms - 0.3 * 8 + 0.5) return fail("outer self time excludes inner");
  }
  if (p.avg_frame_ms() < 0.7 || p.fps() <= 0.0) return fail("frame time / fps");
  if (p.latest("render.draws") != 7 || p.latest("nope", -1) != -1) return fail("latest()");
  p.set_history(600);
  std::cout << "[smoke] profiler core OK (nesting, ring, counters, gpu, stats, fps)\n";
  return 0;
}

int capture_checks(const fs::path& dir) {
  Profiler& p = Profiler::get();
  p.set_capture_dir((dir / "captures").string());
  std::string err;
  const std::string path = p.capture(&err, "round_trip", 4);
  if (path.empty()) return fail("capture: " + err);
  if (fs::path(path).filename() != "round_trip.tsprof.json") return fail("capture name " + path);
  std::ifstream in(path);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (text.find("\"traceEvents\"") == std::string::npos || text.find("\"tombstone\"") == std::string::npos)
    return fail("capture should be Chrome trace JSON with a tombstone block");
  std::vector<profiler::FrameRecord> back;
  if (!profiler::load_capture(path, &back, &err)) return fail("load capture: " + err);
  if (back.size() != 4) return fail("capture should hold 4 frames");
  if (back.back().zones.size() != 3 || back.back().counters["render.draws"] != 7 ||
      back.back().gpu_ms["opaque"] != 1.5)
    return fail("capture round trip lost data: zones " + std::to_string(back.back().zones.size()) + " draws " + std::to_string(back.back().counters["render.draws"]) + " gpu " + std::to_string(back.back().gpu_ms["opaque"]));
  if (profiler::zone_stats(back).empty()) return fail("stats over a loaded capture");
  std::vector<profiler::FrameRecord> junk;
  if (profiler::parse_capture("{\"nope\":1}", &junk, &err)) return fail("junk capture should fail");
  // Panel helpers.
  std::vector<float> series;
  if (editor::counter_series(back, "render.draws", &series) != 28 || series.size() != 4)
    return fail("counter_series");
  if (editor::counter_names(back, "render.").size() != 1) return fail("counter_names");
  // A 0.1 ms tick inside a 40 ms frame zooms far enough to read.
  if (editor::timeline_zoom_for(40.0, 0.1) < 200.0f || editor::timeline_zoom_for(40.0, 40.0) != 1.0f)
    return fail("timeline_zoom_for");
  std::cout << "[smoke] profiler capture OK (" << fs::path(path).filename().string() << ", 4 frames round trip)\n";
  return 0;
}

int console_checks() {
  profiler::Console& c = profiler::Console::shared();
  for (const char* cmd : {"help", "echo", "prof", "capture", "fps", "scopes", "counters", "gpu", "mem"})
    if (!c.has(cmd)) return fail(std::string("console command missing: ") + cmd);
  if (c.run("echo howdy partner") != "howdy partner") return fail("echo");
  if (c.run("fps").find("fps") == std::string::npos) return fail("fps output");
  if (c.run("scopes 3").find("outer") == std::string::npos) return fail("scopes output");
  if (c.run("counters render.").find("render.draws") == std::string::npos) return fail("counters output");
  if (c.run("prof off"); Profiler::get().enabled()) return fail("prof off");
  c.run("prof on");
  if (!Profiler::get().enabled()) return fail("prof on");
  if (c.run("capture console_cap 2").find("console_cap.tsprof.json") == std::string::npos)
    return fail("capture command");
  if (c.run("whiskey").find("nknown") == std::string::npos) return fail("unknown command message");
  int custom = 0;
  c.add("ring", "ring the bell", [&](const std::vector<std::string>& a) {
    custom = static_cast<int>(a.size());
    return std::string("ding");
  });
  if (c.run("ring twice now") != "ding" || custom != 3) return fail("custom command args");
  c.remove("ring");
  if (c.has("ring")) return fail("remove");
  std::cout << "[smoke] profiler console OK (" << c.names().size() << " commands)\n";
  return 0;
}

int debug_tools_checks() {
  ui::UiSystem ui;
  {
    profiler::DebugTools dt;
    dt.attach(ui);
    if (!ui.has("debug_overlay") || !ui.has("debug_console")) return fail("debug docs missing");
    if (!profiler::Console::shared().has("overlay")) return fail("overlay command");
    dt.set_overlay(true);
    Profiler::get().begin_frame();
    Profiler::get().end_frame();
    dt.update(0.5);
    if (!ui.visible("debug_overlay")) return fail("overlay should show");
    const ui::Widget* fps = ui.find("ov_fps");
    if (!fps || fps->text.find("fps") == std::string::npos) return fail("overlay fps text");
    const std::string ov = profiler::DebugTools::overlay_text();
    if (ov.find("ms") == std::string::npos) return fail("overlay text");
    dt.set_console(true);
    if (!ui.visible("debug_console")) return fail("console should show");
    profiler::Console::shared().run("overlay off");
    if (dt.overlay()) return fail("overlay command should hide it");
    dt.toggle_console();
    if (ui.visible("debug_console")) return fail("console toggle");
  }
  if (profiler::Console::shared().has("overlay")) return fail("overlay command should go with the tools");
  std::cout << "[smoke] profiler debug tools OK (overlay, console, commands)\n";
  return 0;
}

int world_checks(const fs::path& dir) {
  plugin::Catalog cat;
  plugins::register_builtin_plugins(cat, nullptr);
  const plugin::PluginInfo* info = cat.find("ts.profiler") ? &cat.find("ts.profiler")->info : nullptr;
  if (!info) return fail("ts.profiler should be a built-in");
  if (!plugins::profiler_enabled({"ts.profiler"}) || plugins::profiler_enabled({"ts.story"}))
    return fail("profiler_enabled");
  plugin::PluginManager m(cat);
  plugin::PluginContext ctx;
  ctx.project_dir = dir.string();
  m.load({"ts.profiler"}, ctx);
  if (!m.loaded("ts.profiler")) return fail("ts.profiler should load");
  Profiler& p = Profiler::get();
  p.set_enabled(false);
  p.clear();
  runtime::World w;
  w.set_extensions(m.world_extensions());
  std::vector<Entity2D> ents;
  Entity2D e;
  e.id = 1;
  e.name = "Assayer";
  e.w = e.h = 16;
  e.script = ScriptData{};
  e.script->path = "scripts/assay.lua";
  ents.push_back(e);
  w.script_library().put("scripts/assay.lua", R"(
    function on_tick()
      profiler.begin("assay")
      profiler.counter("gold", 42)
      profiler.finish()
      if console.run("echo hi") == "hi" then self:set("console", "ok") end
    end
  )");
  std::string err;
  if (!w.build(ents, dir.string(), &err)) return fail("2D build: " + err);
  m.play_begin(w);
  if (!p.enabled()) return fail("Play should switch the profiler on");
  if (p.capture_dir() != plugins::capture_dir_for(dir.string())) return fail("capture dir from project");
  runtime::InputFrame in;
  for (int i = 0; i < 5; ++i) {
    p.begin_frame();
    w.step(in);
    p.end_frame();
  }
  auto frames = p.frames();
  for (const char* z : {"world.step", "world.scripts", "scripts.lua", "lua.assay"})
    if (!has_zone(frames, z)) {
      for (const auto& l : w.take_logs()) std::cerr << "  " << runtime::format_log(l) << "\n";
      return fail(std::string("2D zone missing: ") + z);
    }
  if (p.latest("script.gold") != 42 || p.latest("world.entities") < 1) return fail("2D counters");
  m.play_end();
  m.unload_all();
  std::cout << "[smoke] profiler 2D OK (" << frames.back().zones.size() << " zones/frame, Lua profiler + console)\n";

  // 3D.
  if (!scene3d::write_ghost_town_sample((dir / "w3d").string(), "Assay 3D", &err)) return fail(err);
  scene3d::Scene3D scene;
  if (!scene3d::load_file(scene3d::scene_path((dir / "w3d").string()), &scene, &err)) return fail(err);
  world3d::World3D world;
  profiler::attach_profiler(world);
  if (!profiler::find_profiler(world)) return fail("find_profiler");
  p.clear();
  if (!world.start(scene, (dir / "w3d").string(), &err)) return fail("3D start: " + err);
  if (!p.enabled()) return fail("3D start should switch the profiler on");
  for (int i = 0; i < 5; ++i) {
    p.begin_frame();
    world.run_ticks(1, {});
    p.end_frame();
  }
  frames = p.frames();
  for (const char* z : {"world3d.step", "world3d.physics", "world3d.scripts"})
    if (!has_zone(frames, z)) return fail(std::string("3D zone missing: ") + z);
  if (p.latest("world.entities") < 1) return fail("3D entity counter");
  world.stop();
  std::cout << "[smoke] profiler 3D OK\n";
  return 0;
}

int panel_checks(const fs::path& dir) {
  std::string err;
  if (!scene3d::write_ghost_town_sample((dir / "p3d").string(), "Assay Editor", &err)) return fail(err);
  editor::ProjectInfo info;
  info.id = "_profiler_editor";
  info.name = "Assay Editor";
  info.kind = editor::ProjectKind::ThreeD;
  info.path = (dir / "p3d").string();
  info.plugins = {"ts.profiler"};
  info.plugins_set = true;
  editor::Editor3DScreen e(info);
  e.on_enter();
  if (!e.plugins().loaded("ts.profiler")) return fail("editor: ts.profiler should load");
  int panels = 0;
  for (const auto& pp : e.plugins().manager().editor_panels())
    panels += pp.item.id == "profiler.assay_office" || pp.item.id == "profiler.render_memory" ||
              pp.item.id == "profiler.net";
  if (panels != 3) return fail("editor: the three profiler panels");
  if (!e.play_start()) return fail("editor: Play");
  Profiler& p = Profiler::get();
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGuiContext* c = ImGui::CreateContext();
  ImGui::SetCurrentContext(c);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280, 800);
  io.IniFilename = nullptr;
  unsigned char* px = nullptr;
  int fw = 0, fh = 0;
  io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
  std::map<std::string, bool> open;
  bool shown = false;
  for (int frame = 0; frame < 3; ++frame) {
    p.begin_frame();
    { TS_PROFILE_SCOPE("editor.frame"); spin_ms(0.2); }
    p.counter("net.rtt_ms", 42);
    p.end_frame();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    for (const char* id : {"profiler.assay_office", "profiler.render_memory", "profiler.net"}) open[id] = true;
    editor::draw_open_plugin_panels(e.plugins(), open);
    auto active = [](const char* n) {
      ImGuiWindow* w = ImGui::FindWindowByName(n);
      return w && w->Active;
    };
    shown = active("Assay Office###plugin_profiler.assay_office") &&
            active("Render & Memory###plugin_profiler.render_memory") && active("Net###plugin_profiler.net");
    ImGui::Render();
  }
  ImGui::DestroyContext(c);
  ImGui::SetCurrentContext(prev);
  if (!shown) return fail("editor: panels should draw during Play");
  if (!p.enabled()) return fail("editor: Play should record");
  if (!has_zone(p.frames(), "editor.frame")) return fail("editor: frames recorded");
  e.play_stop();
  e.on_exit();
  std::cout << "[smoke] profiler editor OK (Assay Office, Render & Memory, Net during Play)\n";
  return 0;
}

}  // namespace

int run_profiler_smoke() {
  const fs::path root = fs::absolute("./TombStoneProjects/_profiler_smoke");
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  if (core_checks() != 0) return 1;
  if (capture_checks(root) != 0) return 1;
  if (console_checks() != 0) return 1;
  if (debug_tools_checks() != 0) return 1;
  if (world_checks(root / "worlds") != 0) return 1;
  if (panel_checks(root / "editor") != 0) return 1;
  Profiler::get().set_enabled(false);
  Profiler::get().clear();
  fs::remove_all(root, ec);
  std::cout << "[smoke] profiler OK\n";
  return 0;
}
