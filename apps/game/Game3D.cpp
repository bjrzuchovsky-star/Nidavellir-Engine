#include "audio/GameAudio.h"
#include "debugtools/ProfilerWorld3D.h"
#include "profiler/Profiler.h"
#include "plugins/profiler/ProfilerPlugin.h"
#include "audio/SoundStage3D.h"
#include "Game3D.h"
#include "GameManifest.h"
#include "GpuCheck.h"
#include "GameUi.h"

#include "render3d/GlRenderer3D.h"
#include "render3d/HiddenGl.h"
#include "render3d/Scene3D.h"
#include "render3d/SceneView.h"
#include "runtime/Log.h"
#include "ui/UiScript.h"
#include "world3d/World3D.h"
#include "ai3d/AiWorld3D.h"
#include "plugins/story/StoryPlugins.h"
#include "story3d/StoryWorld3D.h"
#include "weather/Weather3D.h"
#include "plugins/RuntimePlugins.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace ts {
namespace tombstone {
namespace game {

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void print_log(world3d::World3D& w, std::size_t* seen) {
  const auto& log = w.log();
  if (*seen > log.size()) *seen = 0;
  for (std::size_t i = *seen; i < log.size(); ++i) {
    std::cout << runtime::format_log(log[i]) << '\n';
  }
  *seen = log.size();
}

// Scroll steps since the last frame (third-person zoom).
double g_scroll = 0.0;
void on_scroll_3d(GLFWwindow*, double, double dy) { g_scroll += dy; }

float stick_deadzone(float v) {
  constexpr float kDead = 0.2f;
  if (std::fabs(v) < kDead) return 0.0f;
  const float s = (std::fabs(v) - kDead) / (1.0f - kDead);
  return v < 0.0f ? -s : s;
}

// Mouse look: hide + lock the cursor (raw motion where the platform has it).
void capture_cursor(GLFWwindow* win, bool on) {
  glfwSetInputMode(win, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
  if (glfwRawMouseMotionSupported()) {
    glfwSetInputMode(win, GLFW_RAW_MOUSE_MOTION, on ? GLFW_TRUE : GLFW_FALSE);
  }
}

int fail(const std::string& what) {
  std::cerr << "[smoke] ts_game 3D FAILED: " << what << '\n';
  return 1;
}

}  // namespace

// ts.ai: the project lists it in project.json "plugins".
// ts.quests / ts.dialogue / ts.saves: the story runtime rides along.
story::StoryFeatures story_enabled(const std::string& dir) {
  return plugins::story_features(plugins::read_project_plugins(dir).ids);
}

bool weather_enabled(const std::string& dir) {
  for (const std::string& id : plugins::read_project_plugins(dir).ids) {
    if (id == "ts.weather") return true;
  }
  return false;
}

bool ai_enabled(const std::string& dir) {
  for (const std::string& id : plugins::read_project_plugins(dir).ids) {
    if (id == "ts.ai") return true;
  }
  return false;
}

bool is_3d_project(const std::string& dir) {
  const fs::path scene = scene3d::scene_path(dir);
  std::error_code ec;
  if (fs::exists(scene, ec)) return scene3d::is_3d_scene_text(slurp(scene));
  const std::string pj = slurp(fs::path(dir) / "project.json");
  return pj.find("\"3d\"") != std::string::npos;
}

int run_smoke_3d(const std::string& dir, const std::string& name) {
  const std::string path = scene3d::scene_path(dir);
  const std::string before = slurp(path);
  scene3d::Scene3D scene;
  std::string err;
  if (!scene3d::load_file(path, &scene, &err)) return fail(err);
  world3d::World3D world;
  ai3d::attach(world, ai_enabled(dir));
  story3d::attach(world, story_enabled(dir));
  weather::attach(world, weather_enabled(dir));
  if (!world.start(scene, dir, &err)) return fail("start: " + err);
  if (world.player_id() == 0) return fail("no rider (Player component) in the scene");
  const world3d::Entity3D* rider = world.find(world.player_id());
  const render3d::Vec3 start = rider->transform.position;
  world.run_ticks(60, {});
  world3d::Input3D in;
  in.move_forward = 1.0f;
  world.run_ticks(60, in);
  rider = world.find(world.player_id());
  const render3d::Vec3 end = rider->transform.position;
  const float moved = render3d::length(render3d::Vec3{end.x - start.x, 0.0f, end.z - start.z});
  if (moved < 0.5f) return fail("the rider did not move (" + std::to_string(moved) + " m)");
  if (!world.grounded()) return fail("the rider is not on the ground after a ride");
  std::size_t errors = 0;
  for (const auto& e : world.log()) errors += e.level == runtime::LogLevel::Error ? 1 : 0;
  if (errors) return fail(std::to_string(errors) + " script error(s) on the Telegraph");
  if (slurp(path) != before) return fail("scene.json changed by a ride");

  // One GL frame through the follow camera.
  std::string gl_note = "GL skipped (no display)";
  render3d::HiddenGl ctx;
  std::string why;
  if (ctx.open(&why)) {
    auto r = render3d::make_gl_renderer3d();
    if (!r->init(&why)) return fail("GL renderer: " + why);
    r->set_offscreen(true);
    const int W = 256, H = 144;
    render3d::FrameStats st;
    {
      render3d::SceneView view(r.get());
      view.set_project_dir(dir);
      view.begin(world.environment(), world.entities(), world.camera(), W, H);
      st = view.end();
      for (const std::string& p : view.take_problems()) return fail("render: " + p);
    }
    std::vector<unsigned char> px;
    int w = 0, h = 0;
    if (!r->read_pixels(&px, &w, &h) || w != W || h != H) return fail("read_pixels");
    // The rider sits mid-screen in front of the sky: the frame must not be
    // one flat colour, and the middle must differ from the top row.
    int lo = 255, hi = 0;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
      const int l = (px[i] * 3 + px[i + 1] * 6 + px[i + 2]) / 10;
      lo = std::min(lo, l);
      hi = std::max(hi, l);
    }
    if (st.draws == 0 || hi - lo < 40) {
      return fail("GL frame looks empty (" + std::to_string(st.draws) + " draws, lum " +
                  std::to_string(lo) + ".." + std::to_string(hi) + ")");
    }
    char buf[200];
    std::snprintf(buf, sizeof(buf), "GL %zu draws %zu tris %zu lights, lum %d..%d", st.draws,
                  st.triangles, st.lights, lo, hi);
    gl_note = buf;
    r->shutdown();
  } else if (std::getenv("TS_SMOKE_REQUIRE_GL")) {
    return fail("GL context: " + why);
  }
  std::cout << "[smoke] ts_game 3D OK (\"" << name << "\": " << world.entities().size()
            << " entities, " << world.static_colliders() << " static colliders, "
            << world.script_count() << " scripts, rode " << moved << " m, grounded, "
            << gl_note << ", scene.json untouched)\n";
  return 0;
}

int run_window_3d(const std::string& dir, const std::string& name, long frames) {
  scene3d::Scene3D scene;
  std::string err;
  if (!scene3d::load_file(scene3d::scene_path(dir), &scene, &err)) {
    std::cerr << "[ts_game] " << err << '\n';
    return 1;
  }
  if (!glfwInit()) {
    std::cerr << "[ts_game] could not start GLFW\n";
    return 1;
  }
  render3d::hint_gl33_core();
  GLFWwindow* win = glfwCreateWindow(game::window_width(), game::window_height(), (name + " | TombStone 3D").c_str(), nullptr,
                                     nullptr);
  if (!win) {
    std::cerr << "[ts_game] could not open a GL 3.3 core window\n";
    game::show_gpu_error(name, game::last_glfw_error());
    glfwTerminate();
    return 1;
  }
  game::apply_window_icon(win, dir);
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);
  int code = 0;
  {
    auto renderer = render3d::make_gl_renderer3d();
    world3d::World3D world;
    ai3d::attach(world, ai_enabled(dir));
    story3d::attach(world, story_enabled(dir));
    weather::attach(world, weather_enabled(dir));
    const bool prof = plugins::profiler_enabled(plugins::read_project_plugins(dir).ids);
    if (prof) {
      profiler::attach_profiler(world);
      profiler::Profiler::get().set_capture_dir(plugins::capture_dir_for(dir));
    }
    if (!renderer->init(&err) || !world.start(scene, dir, &err)) {
      std::cerr << "[ts_game] " << err << '\n';
      code = 1;
    } else {
      renderer->set_offscreen(false);
      render3d::SceneView view(renderer.get());
      std::vector<render3d::Renderer3D::Billboard> bills;
      view.set_project_dir(dir);
      std::cout << "[ts_game] riding \"" << name << "\" in 3D: " << world.entities().size()
                << " entities\n";
      audio::GameAudio sound(dir, true, "ts_game");
      audio::SoundStage3D sound3d(sound.stage());
      std::size_t seen = 0;
      double last = glfwGetTime(), scripts_at = last;
      bool prev_action = false, prev_pause = false, prev_throw = false;
      long n = 0;
      double mx0 = 0, my0 = 0;
      glfwGetCursorPos(win, &mx0, &my0);
      GameUi gui;
      gui.load(dir);
      gui.attach(win);
      // Mouse look: the cursor is captured while riding; Esc (the pause
      // menu) or losing focus lets it go, a click in the window (or
      // resuming from the menu) takes it back.
      bool captured = false, swallow_click = false, was_menu = false;
      auto set_captured = [&](bool on) {
        if (on == captured) return;
        captured = on;
        capture_cursor(win, on);
        glfwGetCursorPos(win, &mx0, &my0);  // no jump on the next delta
      };
      if (world.wants_cursor_capture()) {
        set_captured(true);
        std::cout << "[ts_game] mouse look: cursor captured (Esc frees it, click to look again)\n";
      }
      g_scroll = 0.0;
      glfwSetScrollCallback(win, on_scroll_3d);
      gui.enable_debug_tools(prof);
      bool menu_paused = false;
      double ui_last = glfwGetTime();
      while (!glfwWindowShouldClose(win)) {
        profiler::Profiler::get().begin_frame();
        glfwPollEvents();
        {
          // In-game UI: Esc / pad Start pause menu, Lua ui.* both ways.
          const double t = glfwGetTime();
          const GameUi::Frame uf = gui.update(win, t - ui_last, &sound.engine());
          ui_last = t;
          if (uf.quit) break;
          if (gui.pause_open() && !world.paused()) {
            world.set_paused(true);
            menu_paused = true;
          } else if (!gui.pause_open() && menu_paused) {
            world.set_paused(false);
            menu_paused = false;
          }
          ui::forward_ui_events(world, uf.events);
        }
        const bool keys_free = !gui.wants_input();
        auto key = [&](int k) { return keys_free && glfwGetKey(win, k) == GLFW_PRESS; };
        const bool focused = glfwGetWindowAttrib(win, GLFW_FOCUSED) == GLFW_TRUE;
        const bool menu = gui.pause_open() || gui.wants_input();
        const bool lmb = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (captured && (menu || !focused)) set_captured(false);
        if (!captured && world.wants_cursor_capture() && focused && !menu && (was_menu || lmb)) {
          set_captured(true);
          swallow_click = lmb;  // the click that took the cursor back is not a grab
        }
        was_menu = menu;
        if (!lmb) swallow_click = false;
        world3d::Input3D in;
        in.move_right = (key(GLFW_KEY_D) || key(GLFW_KEY_RIGHT) ? 1.0f : 0.0f) -
                        (key(GLFW_KEY_A) || key(GLFW_KEY_LEFT) ? 1.0f : 0.0f);
        in.move_forward = (key(GLFW_KEY_W) || key(GLFW_KEY_UP) ? 1.0f : 0.0f) -
                          (key(GLFW_KEY_S) || key(GLFW_KEY_DOWN) ? 1.0f : 0.0f);
        // Keyboard turn works in both look modes (the fallback).
        in.turn = (key(GLFW_KEY_Q) ? 2.0f : 0.0f) - (key(GLFW_KEY_Z) ? 2.0f : 0.0f);
        double mx = 0, my = 0;
        glfwGetCursorPos(win, &mx, &my);
        const float dx = static_cast<float>(mx - mx0), dy = static_cast<float>(my - my0);
        if (world.mouse_look()) {
          // Captured: every count looks. Capture off: the free cursor looks
          // while it moves over the focused window.
          if (!menu && focused && (captured || !world.wants_cursor_capture())) {
            in.mouse_dx = dx;
            in.mouse_dy = dy;
          }
        } else if (glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
          in.turn -= dx * 0.3f;  // Keys look: right-drag turns
          in.look -= dy * 0.3f;
        }
        in.look += (key(GLFW_KEY_PAGE_UP) ? 2.0f : 0.0f) - (key(GLFW_KEY_PAGE_DOWN) ? 2.0f : 0.0f);
        mx0 = mx;
        my0 = my;
        if (!menu) in.zoom = static_cast<float>(g_scroll);
        g_scroll = 0.0;
        // Pad 1: left stick walks, right stick looks, A jumps.
        GLFWgamepadstate pad;
        if (keys_free && glfwGetGamepadState(GLFW_JOYSTICK_1, &pad)) {
          const float lx = stick_deadzone(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
          const float ly = stick_deadzone(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
          if (std::fabs(lx) > std::fabs(in.move_right)) in.move_right = lx;
          if (std::fabs(ly) > std::fabs(in.move_forward)) in.move_forward = -ly;
          in.stick_x = stick_deadzone(pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_X]);
          in.stick_y = stick_deadzone(pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]);
          in.jump = pad.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS;
        }
        in.jump = in.jump || key(GLFW_KEY_SPACE);
        const bool action = key(GLFW_KEY_E);
        in.action = action && !prev_action;
        prev_action = action;
        // Physics gun: hold F (or the left mouse button) to carry the body
        // ahead; G throws it.
        in.grab = key(GLFW_KEY_F) || (keys_free && lmb && !swallow_click);
        const bool throw_key = key(GLFW_KEY_G);
        in.throw_held = throw_key && !prev_throw;
        prev_throw = throw_key;
        const bool pause = key(GLFW_KEY_P);
        if (pause && !prev_pause) world.set_paused(!world.paused());
        prev_pause = pause;
        if (key(GLFW_KEY_F5)) {
          scene3d::Scene3D fresh;
          if (scene3d::load_file(scene3d::scene_path(dir), &fresh, &err) &&
              world.start(fresh, dir, &err)) {
            if (!world.wants_cursor_capture()) set_captured(false);
            view.clear();
            sound.restart(dir, true);
            sound3d.reset();
            gui.load(dir);
            std::cout << "[ts_game] reloaded scene.json\n";
          }
        }
        const double now = glfwGetTime();
        {
          TS_PROFILE_SCOPE("game.update");
          world.update(now - last, in);
        }
        sound3d.update(world, now - last);
        sound.tick(now - last);
        last = now;
        if (now - scripts_at >= 0.5) {
          scripts_at = now;
          world.reload_scripts();
        }
        for (const world3d::Toast3D& t : world.take_toasts()) {
          std::cout << "[toast] " << t.text << '\n';
          gui.toast(t.text, t.seconds);
        }
        ui::apply_ui_commands(gui.system(), world.take_ui_commands());
        print_log(world, &seen);
        int fw = 0, fh = 0;
        glfwGetFramebufferSize(win, &fw, &fh);
        if (fw > 0 && fh > 0) {
          std::uint64_t version = 0;
          const std::vector<unsigned char>& overlay = gui.raster(fw, fh, &version);
          renderer->set_overlay(overlay.empty() ? nullptr : overlay.data(), fw, fh, version);
          view.begin(world.environment(), world.entities(), world.camera(), fw, fh);
          bills.clear();
          world.billboards(&bills);
          view.billboards(bills);
          view.end();
          for (const std::string& p : view.take_problems()) std::cerr << "[ts_game] " << p << '\n';
          glfwSwapBuffers(win);
        }
        if (profiler::Profiler::get().enabled() && n % 15 == 0) {
          profiler::Profiler::get().counter("mem.process_mb", profiler::process_memory_mb());
        }
        profiler::Profiler::get().end_frame();
        if (frames >= 0 && ++n >= frames) break;
      }
      glfwSetScrollCallback(win, nullptr);
      set_captured(false);
      gui.detach(win);
      std::cout << "[ts_game] rode " << world.tick() << " ticks. So long.\n";
      world.stop();
    }
    renderer->shutdown();
  }
  glfwDestroyWindow(win);
  glfwTerminate();
  return code;
}

}  // namespace game
}  // namespace tombstone
}  // namespace ts
