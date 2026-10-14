// ts_game for "dimension": "vr" projects: the scene in World3D VR mode
// through ts_xr (OpenXR headset, else the desktop simulator), a desktop
// mirror window, the headless VR smoke and the --xr-probe check.

#include "Game3D.h"
#include "GameUi.h"

#include "core/JsonMini.h"
#include "core/MemImages.h"
#include "render3d/GlApi.h"
#include "render3d/GlRenderer3D.h"
#include "render3d/HiddenGl.h"
#include "render3d/Scene3D.h"
#include "render3d/SceneView.h"
#include "runtime/Log.h"
#include "ui/UiScript.h"
#include "world3d/VrOverlay.h"
#include "world3d/World3D.h"
#include "xr/VrPresenter.h"
#include "xr/XrSession.h"

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
using render3d::Vec3;

namespace {

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

int fail(const std::string& what) {
  std::cerr << "[smoke] ts_game VR FAILED: " << what << '\n';
  return 1;
}

void print_log(world3d::World3D& w, std::size_t* seen) {
  const auto& log = w.log();
  if (*seen > log.size()) *seen = 0;
  for (std::size_t i = *seen; i < log.size(); ++i) {
    std::cout << runtime::format_log(log[i]) << '\n';
  }
  *seen = log.size();
}

// Luminance range and how many pixels differ between two eyes.
void eye_stats(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b, int* lo,
               int* hi, std::size_t* differ) {
  *lo = 255;
  *hi = 0;
  *differ = 0;
  for (std::size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4) {
    const int l = (a[i] * 3 + a[i + 1] * 6 + a[i + 2]) / 10;
    *lo = std::min(*lo, l);
    *hi = std::max(*hi, l);
    if (std::abs(a[i] - b[i]) + std::abs(a[i + 1] - b[i + 1]) + std::abs(a[i + 2] - b[i + 2]) > 24) {
      ++*differ;
    }
  }
}

xr::SimDrive glfw_drive(GLFWwindow* win, double* last_x, double* last_y) {
  xr::SimDrive d;
  auto key = [&](int k) { return glfwGetKey(win, k) == GLFW_PRESS; };
  auto mouse = [&](int b) { return glfwGetMouseButton(win, b) == GLFW_PRESS; };
  double mx = 0, my = 0;
  glfwGetCursorPos(win, &mx, &my);
  if (mouse(GLFW_MOUSE_BUTTON_RIGHT)) {
    d.look_dx = static_cast<float>(mx - *last_x);
    d.look_dy = static_cast<float>(my - *last_y);
  }
  *last_x = mx;
  *last_y = my;
  d.forward = key(GLFW_KEY_W);
  d.back = key(GLFW_KEY_S);
  d.left = key(GLFW_KEY_A);
  d.right = key(GLFW_KEY_D);
  d.turn_left = key(GLFW_KEY_Q);
  d.turn_right = key(GLFW_KEY_E);
  d.teleport = key(GLFW_KEY_T);
  d.trigger = mouse(GLFW_MOUSE_BUTTON_LEFT);
  d.grip = key(GLFW_KEY_G) || mouse(GLFW_MOUSE_BUTTON_MIDDLE);
  d.left_hand = key(GLFW_KEY_LEFT_SHIFT) || key(GLFW_KEY_RIGHT_SHIFT);
  d.menu = key(GLFW_KEY_M) || key(GLFW_KEY_TAB);
  d.crouch = key(GLFW_KEY_LEFT_CONTROL);
  d.reach_out = key(GLFW_KEY_X);
  d.reach_in = key(GLFW_KEY_Z);
  return d;
}

// Tracking-space pose for a world point (hands placed by the smoke).
world3d::VrPose tracked_at(const world3d::World3D& w, const Vec3& world_pos) {
  world3d::VrPose p;
  p.position = w.rig_matrix().inverse().transform_point(world_pos);
  p.orientation = render3d::Quat::axis_angle({0, 1, 0}, 0.0f);
  p.valid = true;
  return p;
}

}  // namespace

bool is_vr_project(const std::string& dir) {
  const std::string pj = slurp(fs::path(dir) / "project.json");
  if (auto obj = json_mini::parse_object(pj)) {
    return json_mini::get_string(*obj, "dimension", "") == "vr";
  }
  return false;
}

int run_smoke_vr(const std::string& dir, const std::string& name) {
  const std::string path = scene3d::scene_path(dir);
  const std::string before = slurp(path);
  scene3d::Scene3D scene;
  std::string err;
  if (!scene3d::load_file(path, &scene, &err)) return fail(err);
  world3d::World3D world;
  world.set_vr(true);
  if (!world.start(scene, dir, &err)) return fail("start: " + err);
  if (world.vr_rig_id() == 0) return fail("no VR Rig in the scene");

  render3d::HiddenGl ctx;
  std::string why;
  const bool gl = ctx.open(&why);
  if (!gl && std::getenv("TS_SMOKE_REQUIRE_GL")) return fail("GL context: " + why);
  auto r = gl ? render3d::make_gl_renderer3d() : render3d::make_null_renderer3d();
  if (!r->init(&why)) return fail("renderer: " + why);
  xr::XrConfig cfg;
  cfg.sim_eye_width = 256;
  cfg.sim_eye_height = 256;
  auto sim = xr::create_sim_session(cfg);
  if (!sim->begin(&err)) return fail("simulator: " + err);
  std::string gl_note = "GL skipped (no display)";
  int code = 0;
  {
    render3d::SceneView view(r.get());
    view.set_project_dir(dir);
    xr::VrPresenter presenter(r.get(), &view);
    xr::VrFrameResult res;
    auto run = [&](int frames) {
      for (int i = 0; i < frames; ++i) presenter.frame(*sim, world, 1.0 / 60.0, &res);
    };
    xr::SimXrSession::Controls& c = sim->controls();
    run(30);
    // Stereo: both eyes drawn, scene visible, and the eyes differ (IPD).
    if (gl) {
      std::vector<unsigned char> le, re;
      int w = 0, h = 0;
      if (!sim->read_eye(0, &le, &w, &h) || !sim->read_eye(1, &re, &w, &h)) {
        code = fail("read eyes");
      }
      int lo = 0, hi = 0;
      std::size_t differ = 0;
      eye_stats(le, re, &lo, &hi, &differ);
      if (!code && (res.eyes[0].draws == 0 || res.eyes[1].draws == 0 || hi - lo < 30 ||
                    differ < le.size() / 4 / 200)) {
        code = fail("stereo frame (" + std::to_string(res.eyes[0].draws) + " draws, lum " +
                    std::to_string(lo) + ".." + std::to_string(hi) + ", " +
                    std::to_string(differ) + " px differ)");
      }
      char buf[160];
      std::snprintf(buf, sizeof(buf), "stereo %dx%d x2: %zu draws/eye, lum %d..%d, %zu px differ",
                    w, h, res.eyes[0].draws, lo, hi, differ);
      gl_note = buf;
    }
    // Smooth locomotion (left stick forward).
    const Vec3 r0 = world.rig_position();
    c.buttons[world3d::kLeftHand].stick_y = 1.0f;
    if (!code) run(60);
    c.buttons[world3d::kLeftHand].stick_y = 0.0f;
    const float walked = render3d::length(world.rig_position() - r0);
    if (!code && walked < 0.5f) code = fail("smooth locomotion moved " + std::to_string(walked) + " m");
    // Snap turn (right stick).
    const float yaw0 = world.rig_yaw();
    c.buttons[world3d::kRightHand].stick_x = 1.0f;
    if (!code) run(4);
    c.buttons[world3d::kRightHand].stick_x = 0.0f;
    if (!code) run(4);
    const float turned = world.rig_yaw() - yaw0;
    if (!code && std::fabs(turned + world.vr_settings().snap_turn_deg) > 0.5f) {
      code = fail("snap turn: " + std::to_string(turned) + " deg");
    }
    // Grab + throw the first grabbable.
    const scene3d::Entity3D* prop = nullptr;
    for (const auto& e : world.entities()) {
      if (e.grabbable) {
        prop = &e;
        break;
      }
    }
    std::string grab_note = "no grabbables";
    if (!code && prop) {
      const std::uint64_t id = prop->id;
      const std::string prop_name = prop->name;
      Vec3 hand = prop->transform.position;
      c.hands_follow_head = false;
      c.manual = world3d::VrInput{};
      c.manual.head = tracked_at(world, world.head().position);
      c.manual.head.orientation = sim->input().head.orientation;
      auto place = [&](const Vec3& p) {
        c.manual.hands[world3d::kRightHand].grip = tracked_at(world, p);
        c.manual.hands[world3d::kRightHand].aim = tracked_at(world, p);
      };
      place(hand);
      c.buttons[world3d::kRightHand].squeeze = 1.0f;
      run(3);
      if (world.hand(world3d::kRightHand).held != id) {
        code = fail("grab: the right hand did not pick up " + prop_name);
      }
      for (int i = 0; i < 8 && !code; ++i) {
        hand += Vec3{0.0f, 0.06f, -0.06f};
        place(hand);
        run(1);
      }
      c.buttons[world3d::kRightHand].squeeze = 0.0f;
      if (!code) run(1);
      if (!code && (world.hand(world3d::kRightHand).held != 0 || world.vr_stats().throws < 1)) {
        code = fail("throw: release did not throw");
      }
      if (!code) run(300);
      const scene3d::Entity3D* landed = world.find(id);
      if (!code && (world.vr_bodies() != 0 || !landed || landed->transform.position.y < -1.0f)) {
        code = fail("thrown " + prop_name + " did not land");
      }
      if (!code) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "threw %s, landed at (%.1f, %.1f, %.1f)", prop_name.c_str(),
                      static_cast<double>(landed->transform.position.x),
                      static_cast<double>(landed->transform.position.y),
                      static_cast<double>(landed->transform.position.z));
        grab_note = buf;
      }
      c.hands_follow_head = true;
    }
    // Teleport: look down, push the right stick, release (a short hop so it
    // lands on open floor in any sample).
    c.pitch_deg = -55.0f;
    c.buttons[world3d::kRightHand].stick_y = 1.0f;
    if (!code) run(5);
    const bool aimed = world.teleport().aiming && world.teleport().valid;
    const Vec3 before_tp = world.rig_position();
    c.buttons[world3d::kRightHand].stick_y = 0.0f;
    if (!code) run(2);
    if (!code && (!aimed || world.vr_stats().teleports != 1 ||
                  render3d::length(world.rig_position() - before_tp) < 0.5f)) {
      code = fail("teleport arc (aimed " + std::to_string(aimed) + ")");
    }
    std::size_t errors = 0;
    for (const auto& e : world.log()) errors += e.level == runtime::LogLevel::Error ? 1 : 0;
    if (!code && errors) code = fail(std::to_string(errors) + " script error(s) on the Telegraph");
    if (!code && slurp(path) != before) code = fail("scene.json changed by a ride");
    if (!code) {
      std::cout << "[smoke] ts_game VR OK (\"" << name << "\": " << world.entities().size()
                << " entities, " << world.script_count() << " scripts, simulator; " << gl_note
                << "; walked " << walked << " m, snap " << turned << " deg, " << grab_note
                << ", teleported, scene.json untouched)\n";
    }
  }
  sim->end();
  r->shutdown();
  return code;
}

int run_window_vr(const std::string& dir, const std::string& name, long frames, bool simulator) {
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
  GLFWwindow* win = glfwCreateWindow(1280, 720, (name + " | TombStone VR").c_str(), nullptr, nullptr);
  if (!win) {
    std::cerr << "[ts_game] could not open a GL 3.3 core window\n";
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(win);
  int code = 0;
  {
    auto renderer = render3d::make_gl_renderer3d();
    xr::XrConfig cfg;
    cfg.app_name = name;
    std::unique_ptr<xr::XrSession> session;
    std::string why;
    if (!simulator) {
      session = xr::create_openxr_session(cfg);
      if (!session->available()) {
        why = session->unavailable_reason();
        session.reset();
      }
    }
    world3d::World3D world;
    world.set_vr(true);
    if (!renderer->init(&err) || !world.start(scene, dir, &err)) {
      std::cerr << "[ts_game] " << err << '\n';
      code = 1;
    } else {
      if (session && !session->begin(&why)) session.reset();
      if (!session) {
        if (!simulator) std::cout << "[ts_game] no headset: " << why << " -- using the desktop simulator\n";
        session = xr::create_sim_session(cfg);
        session->begin(&err);
      }
      xr::SimXrSession* sim = dynamic_cast<xr::SimXrSession*>(session.get());
      glfwSwapInterval(sim ? 1 : 0);  // the headset paces OpenXR frames
      render3d::SceneView view(renderer.get());
      view.set_project_dir(dir);
      xr::VrPresenter presenter(renderer.get(), &view);
      std::cout << "[ts_game] riding \"" << name << "\" in VR on " << session->system_name() << '\n';
      if (sim) {
        std::cout << "[ts_game] simulator: RMB drag look | WASD move | Q/E snap turn | hold T "
                     "teleport | LMB trigger | G/MMB grab | Shift left hand | M comfort menu | "
                     "Ctrl crouch | Z/X reach | Esc pause menu\n";
      }
      std::size_t seen = 0;
      double last = glfwGetTime(), mx = 0, my = 0;
      glfwGetCursorPos(win, &mx, &my);
      long n = 0;
      // In-game UI on a world-space panel: laid out on a 768x432 canvas,
      // rasterised into "mem:ui/vr_panel", pointed at with either laser
      // (trigger = click). Esc opens the pause menu.
      constexpr int kPanelW = 768, kPanelH = 432;
      GameUi gui;
      gui.load(dir);
      gui.attach(win);
      gui.set_fixed_canvas(kPanelW, kPanelH);
      world3d::VrUiPanel panel;
      bool menu_paused = false;
      std::uint64_t panel_version = 0;
      while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        const double now = glfwGetTime();
        const double dt = now - last;
        last = now;
        {
          bool hit = false, down = false;
          float px = -1, py = -1;
          for (int h = 0; h < 2 && !hit; ++h) {
            float u = 0, v = 0;
            if (world3d::vr_ui_panel_hit(world, panel, h, &u, &v)) {
              hit = true;
              px = u * kPanelW;
              py = v * kPanelH;
              down = world.hand(h).trigger > 0.5f;
            }
          }
          gui.set_virtual_pointer(hit, px, py, down);
          const GameUi::Frame uf = gui.update(win, dt, nullptr);
          if (uf.quit) break;
          if (gui.pause_open() && !world.paused()) {
            world.set_paused(true);
            menu_paused = true;
          } else if (!gui.pause_open() && menu_paused) {
            world.set_paused(false);
            menu_paused = false;
          }
          ui::forward_ui_events(world, uf.events);
          ui::apply_ui_commands(gui.system(), world.take_ui_commands());
          const bool show = gui.anything_visible();
          if (show && !panel.visible) world3d::vr_place_ui_panel(world, 0.9f, &panel);
          panel.visible = show;
          std::vector<scene3d::Entity3D> extra;
          if (show) {
            std::uint64_t ver = 0;
            const std::vector<unsigned char>& px_rgba = gui.raster(kPanelW, kPanelH, &ver);
            if (ver != panel_version && !px_rgba.empty()) {
              panel_version = ver;
              // Opaque over Dusk charcoal: the panel is not blended.
              std::vector<unsigned char> rgba(px_rgba.size());
              const float bg[3] = {21, 19, 18};
              for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
                const float a = px_rgba[i + 3] / 255.0f;
                for (int c = 0; c < 3; ++c) {
                  rgba[i + c] = static_cast<unsigned char>(px_rgba[i + c] * a + bg[c] * (1 - a));
                }
                rgba[i + 3] = 255;
              }
              mem_images::put(panel.image, kPanelW, kPanelH, std::move(rgba));
            }
            world3d::vr_ui_panel_entity(panel, &extra);
          }
          presenter.set_extra_entities(std::move(extra));
        }
        if (sim) sim->drive(glfw_drive(win, &mx, &my), dt);
        if (!presenter.frame(*session, world, dt)) {
          std::cout << "[ts_game] the headset session ended\n";
          break;
        }
        for (const world3d::Toast3D& t : world.take_toasts()) {
          std::cout << "[toast] " << t.text << '\n';
          gui.toast(t.text, t.seconds);
        }
        print_log(world, &seen);
        // Mirror: both eyes side by side, letterboxed.
        int fw = 0, fh = 0;
        glfwGetFramebufferSize(win, &fw, &fh);
        const render3d::Gl33& g = render3d::gl33();
        g.BindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fw, fh);
        glClearColor(0.06f, 0.05f, 0.05f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        const int ew = session->eye_width(), eh = session->eye_height();
        if (fw > 0 && fh > 0 && ew > 0 && eh > 0 && session->eye_framebuffer(0)) {
          const float s = std::min(static_cast<float>(fw) * 0.5f / ew, static_cast<float>(fh) / eh);
          const int dw = static_cast<int>(ew * s), dh = static_cast<int>(eh * s);
          const int y0 = (fh - dh) / 2;
          for (int e = 0; e < 2; ++e) {
            const int x0 = e * (fw / 2) + (fw / 2 - dw) / 2;
            g.BindFramebuffer(GL_READ_FRAMEBUFFER, session->eye_framebuffer(e));
            g.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            g.BlitFramebuffer(0, 0, ew, eh, x0, y0, x0 + dw, y0 + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
          }
          g.BindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        glfwSwapBuffers(win);
        if (frames >= 0 && ++n >= frames) break;
      }
      gui.detach(win);
      std::cout << "[ts_game] rode " << world.tick() << " ticks in VR. So long.\n";
      session->end();
      world.stop();
    }
    renderer->shutdown();
  }
  glfwDestroyWindow(win);
  glfwTerminate();
  return code;
}

int run_xr_probe() {
  std::cout << "[xr-probe] OpenXR loader " << (xr::openxr_built() ? "built in" : "not built") << '\n';
  render3d::HiddenGl ctx;
  std::string why;
  const bool gl = ctx.open(&why);
  if (!gl) std::cout << "[xr-probe] no GL context (" << why << "); skipping session create\n";
  const xr::XrProbe p = xr::probe_openxr(gl);
  std::cout << "[xr-probe] instance: " << (p.instance ? "yes" : "no")
            << (p.runtime.empty() ? "" : " (" + p.runtime + ")") << '\n';
  std::cout << "[xr-probe] headset: " << (p.system ? "yes" : "no")
            << (p.system_name.empty() ? "" : " (" + p.system_name + ")") << '\n';
  std::cout << "[xr-probe] session: " << (p.session ? "created + destroyed" : "not created")
            << (p.swapchain.empty() ? "" : ", swapchains " + p.swapchain) << '\n';
  if (!p.error.empty()) std::cout << "[xr-probe] note: " << p.error << '\n';
  return p.session ? 0 : 1;
}

}  // namespace game
}  // namespace tombstone
}  // namespace ts
