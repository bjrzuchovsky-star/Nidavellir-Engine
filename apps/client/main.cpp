// ts_client: ride with a posse. Connects to a ts_server, sends input every
// tick, predicts its own rider (reconciled against the server's snapshots)
// and draws everybody else interpolated a hair in the past, through the
// same renderer and HUD as ts_game.
//
//   ts_client --connect host:port --name Ike [--project <dir>]
//   ts_client --smoke                          headless loopback check

#include "audio/GameAudio.h"
#include "GameRenderer.h"
#include "GameUi.h"
#include "HudText.h"

#include "gfx/GlTextureUploader.h"
#include "mp/GameClient.h"
#include "mp/PosseSmoke.h"
#include "net/Net.h"
#include "net/Transport.h"
#include "plugins/Builtins.h"
#include "plugins/RuntimePlugins.h"
#include "plugins/split/Split.h"
#include "runtime/Log.h"
#include "runtime/Input.h"
#include "runtime/World.h"
#include "ui/UiScript.h"

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ts::tombstone;
namespace fs = std::filesystem;
using game::GameRenderer;

constexpr const char* kUsage =
    "Usage: ts_client --connect host[:port] [--name NAME] [--project <dir>]\n"
    "                 [--lag MS] [--jitter MS] [--loss PCT] [--interp MS] [--frames N]\n"
    "       ts_client --smoke\n"
    "  --connect host:port  server to ride to (port defaults to 24642)\n"
    "  --name NAME          what the posse calls you (default Stranger)\n"
    "  --project <dir>      local copy of the project for sprites (optional:\n"
    "                       the scene itself comes from the server)\n"
    "  --lag / --jitter MS  simulated one-way latency / jitter on this link\n"
    "  --loss PCT           simulated unreliable packet loss, 0-100\n"
    "  --interp MS          how far behind other riders are drawn (default 100)\n"
    "  --frames N           quit after N frames (testing)\n"
    "  --smoke              headless check: in-process server + 2 clients over\n"
    "                       loopback under lag, convergence, draw list\n"
    "Keys: WASD / arrows ride | E / Space action | gamepad 1 too | Esc menu\n";

struct Options {
  std::string connect;
  std::string name = "Stranger";
  std::string project;
  net::LinkSim sim;
  double interp_ms = 100.0;
  long frames = -1;
  bool smoke = false;
  bool help = false;
};

bool parse_args(int argc, char** argv, Options* o, std::string* err) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has = i + 1 < argc;
    if (a == "--connect" && has) {
      o->connect = argv[++i];
    } else if (a == "--name" && has) {
      o->name = argv[++i];
    } else if (a == "--project" && has) {
      o->project = argv[++i];
    } else if (a == "--lag" && has) {
      o->sim.latency_ms = std::atoi(argv[++i]);
    } else if (a == "--jitter" && has) {
      o->sim.jitter_ms = std::atoi(argv[++i]);
    } else if (a == "--loss" && has) {
      o->sim.loss = static_cast<float>(std::atof(argv[++i]) / 100.0);
    } else if (a == "--interp" && has) {
      o->interp_ms = std::atof(argv[++i]);
    } else if (a == "--frames" && has) {
      o->frames = std::strtol(argv[++i], nullptr, 10);
    } else if (a == "--smoke") {
      o->smoke = true;
    } else if (a == "--help" || a == "-h") {
      o->help = true;
    } else {
      *err = "Unknown or incomplete argument: " + a;
      return false;
    }
  }
  return true;
}

std::string printable(const std::string& s) {
  std::string out;
  for (char c : s) out.push_back(c >= 32 && c < 127 ? c : '?');
  return out;
}

// Camera on our own rider, at the scene camera's zoom.
runtime::CameraView follow_camera(const mp::GameClient& client) {
  runtime::CameraView cam;
  const runtime::World& w = client.world();
  for (const runtime::Actor& a : w.actors()) {
    if (a.data.camera) {
      cam.zoom = std::max(0.05f, a.data.camera->zoom);
      break;
    }
  }
  if (const runtime::Actor* me = w.find(client.rider())) {
    cam.x = me->data.center_x();
    cam.y = me->data.center_y();
  }
  return cam;
}

std::vector<game::HudToast> client_toasts(const mp::GameClient& client) {
  std::vector<game::HudToast> out;
  for (const mp::ClientToast& t : client.toasts()) {
    game::HudToast h;
    h.text = printable(t.slot >= 0 ? "P" + std::to_string(t.slot + 1) + ": " + t.text : t.text);
    const double left = t.until - client.clock();
    h.alpha = left >= 0.5 ? 1.0f : static_cast<float>(std::max(0.0, left) / 0.5);
    out.push_back(std::move(h));
  }
  return out;
}

// --smoke: an in-process posse (server + 2 clients over real ENet loopback,
// 70 ms +- 20 ms, 8% loss), then this client's draw list must show every
// rider where the server has it.
// The 2D physics / movement plugins (0.2.0 split): the smoke's posse claim
// and a client without --project ride with all of them, as before the
// split (the server's world decides; prediction should match it).
void hold_all_2d_providers(plugins::ProviderHold& hold) {
  plugins::offer_builtin_providers();
  hold.clear();
  hold.hold({plugins::kBox2DId, plugins::kTopDownId, plugins::kPlatformerId});
}

int run_smoke() {
  // A local hold (a static one could outlive the provider registry).
  plugins::ProviderHold hold;
  hold_all_2d_providers(hold);
  mp::PosseSmokeOptions opt;
  opt.riders = 2;
  opt.sim = net::LinkSim{70, 20, 0.08f};
  opt.inspect = [](const mp::GameClient& c) -> std::string {
    GameRenderer renderer(c.world().project_dir());
    std::vector<runtime::DrawQuad> quads;
    const runtime::CameraView cam = follow_camera(c);
    c.world().build_draw_list(
        runtime::World::view_rect(cam, 1280.0f, 720.0f), 1.0f,
        [&renderer](const std::string& rel, int* w, int* h) {
          return renderer.image_size(rel, w, h);
        },
        &quads);
    int riders = 0;
    for (const runtime::DrawQuad& q : quads) {
      const runtime::Actor* a = c.world().find(q.entity);
      riders += a && a->data.player ? 1 : 0;
    }
    if (riders < 2) return "!client draw list shows " + std::to_string(riders) + " riders, want 2";
    const auto hud = client_toasts(c);
    return "client draw list " + std::to_string(quads.size()) + " quads (" +
           std::to_string(riders) + " riders), camera on P" + std::to_string(c.slot() + 1) +
           ", " + std::to_string(hud.size()) + " HUD toast(s) up";
  };
  const mp::PosseSmokeResult r = mp::run_posse_smoke(opt);
  if (!r.ok) {
    std::cerr << "[smoke] ts_client FAILED: " << r.failure << '\n';
    return 1;
  }
  std::cout << "[smoke] posse " << r.summary << '\n';
  std::cout << "[smoke] ts_client OK\n";
  return 0;
}

float deadzone(float v) {
  constexpr float kDead = 0.2f;
  if (std::fabs(v) < kDead) return 0.0f;
  const float s = (std::fabs(v) - kDead) / (1.0f - kDead);
  return v < 0.0f ? -s : s;
}

runtime::PlayerInput poll_input(GLFWwindow* win) {
  auto down = [win](int a, int b) {
    return glfwGetKey(win, a) == GLFW_PRESS || glfwGetKey(win, b) == GLFW_PRESS;
  };
  runtime::PlayerInput keys = runtime::digital_input(
      down(GLFW_KEY_A, GLFW_KEY_LEFT), down(GLFW_KEY_D, GLFW_KEY_RIGHT),
      down(GLFW_KEY_W, GLFW_KEY_UP), down(GLFW_KEY_S, GLFW_KEY_DOWN));
  if (down(GLFW_KEY_E, GLFW_KEY_SPACE)) keys.buttons |= runtime::kButtonAction;
  if (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) keys.buttons |= runtime::kButtonAlt;
  if (glfwGetKey(win, GLFW_KEY_ENTER) == GLFW_PRESS) keys.buttons |= runtime::kButtonStart;
  // Frontier Survival (and any plugin that wants them): F / left mouse use,
  // G / right mouse place, C fire, Q / X previous / next hotbar slot.
  if (glfwGetKey(win, GLFW_KEY_F) == GLFW_PRESS ||
      glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
    keys.buttons |= runtime::kButtonUse;
  }
  if (glfwGetKey(win, GLFW_KEY_G) == GLFW_PRESS ||
      glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
    keys.buttons |= runtime::kButtonPlace;
  }
  if (glfwGetKey(win, GLFW_KEY_C) == GLFW_PRESS) keys.buttons |= runtime::kButtonFire;
  if (glfwGetKey(win, GLFW_KEY_X) == GLFW_PRESS) keys.buttons |= runtime::kButtonNext;
  if (glfwGetKey(win, GLFW_KEY_Q) == GLFW_PRESS) keys.buttons |= runtime::kButtonPrev;
  GLFWgamepadstate pad;
  if (glfwGetGamepadState(GLFW_JOYSTICK_1, &pad)) {
    runtime::PlayerInput in;
    in.connected = true;
    in.move_x = deadzone(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
    in.move_y = deadzone(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
    if (pad.buttons[GLFW_GAMEPAD_BUTTON_A]) in.buttons |= runtime::kButtonAction;
    if (pad.buttons[GLFW_GAMEPAD_BUTTON_B]) in.buttons |= runtime::kButtonAlt;
    keys = runtime::merge_input(keys, in);
  }
  return keys;
}

void on_glfw_error(int code, const char* message) {
  std::cerr << "[ts_client] GLFW error " << code << ": " << (message ? message : "") << '\n';
}

int run_window(const Options& opt) {
  std::string host;
  std::uint16_t port = 0;
  if (!net::parse_endpoint(opt.connect, net::kDefaultPort, &host, &port)) {
    std::cerr << "[ts_client] bad --connect \"" << opt.connect << "\" (want host:port)\n";
    return 2;
  }
  // The project's plugins (needs --project: the client reads project.json
  // locally). Declared before the client so they outlive its world.
  plugins::RuntimePlugins rp;
  plugins::ProviderHold hold2d;
  if (!opt.project.empty()) {
    const std::size_t loaded = rp.open(opt.project, "client", [](const plugin::Diagnostic& d) {
      std::cout << "[plugins] [" << runtime::to_string(d.level) << "] "
                << (d.plugin.empty() ? "" : d.plugin + ": ") << d.text << '\n';
    });
    std::cout << "[ts_client] " << loaded << " plugin(s) riding\n";
  } else {
    hold_all_2d_providers(hold2d);
  }
  mp::GameClient client;
  rp.attach(client.world());
  mp::ClientConfig cc;
  cc.host = host;
  cc.port = port;
  cc.name = opt.name;
  cc.project_dir = opt.project;
  cc.sim = opt.sim;
  cc.interp_ms = opt.interp_ms;
  std::string err;
  if (!client.connect(std::move(cc), &err)) {
    std::cerr << "[ts_client] " << err << '\n';
    return 1;
  }
  rp.begin(client.world());
  std::cout << "[ts_client] riding to " << host << ':' << port << " as " << client.name()
            << "...\n";

  glfwSetErrorCallback(on_glfw_error);
  if (!glfwInit()) {
    std::cerr << "[ts_client] could not start GLFW\n";
    return 1;
  }
  GLFWwindow* win = glfwCreateWindow(1280, 720, "TombStone Client | riding in", nullptr, nullptr);
  if (!win) {
    std::cerr << "[ts_client] could not open a window\n";
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);
  gfx::GlTextureUploader uploader;
  gfx::set_texture_uploader(&uploader);
  int code = 0;
  {
    GameRenderer renderer(opt.project);
    std::vector<runtime::DrawQuad> quads;
    auto last = std::chrono::steady_clock::now();
    double title_at = 0.0;
    long frames = 0;
    mp::ClientState shown = mp::ClientState::Idle;
    ts::audio::GameAudio sound(opt.project, false, "ts_client");
    bool digit_down[10] = {};
    // In-game UI: the project's ui/ documents and the pause menu (Esc / pad
    // Start). The posse keeps riding while it is open; it only takes the keys.
    game::GameUi gui;
    gui.load(opt.project);
    gui.attach(win);
    while (!glfwWindowShouldClose(win)) {
      glfwPollEvents();
      const auto now = std::chrono::steady_clock::now();
      const double dt = std::chrono::duration<double>(now - last).count();
      last = now;
      const game::GameUi::Frame uf = gui.update(win, dt, &sound.engine());
      if (uf.quit) break;
      ui::forward_ui_events(client.world(), uf.events);
      ui::apply_ui_commands(gui.system(), client.world().take_ui_commands());
      const bool keys_free = !gui.wants_input();
      for (int k = 0; k < 10 && keys_free; ++k) {
        const bool down = glfwGetKey(win, GLFW_KEY_0 + k) == GLFW_PRESS;
        if (down && !digit_down[k]) {
          plugins::select_hotbar(client.world(), client.slot(), k == 0 ? 9 : k - 1);
        }
        digit_down[k] = down;
      }
      client.update(dt, keys_free ? poll_input(win) : runtime::PlayerInput{});
      if (client.has_snapshot()) {
        // Server-sent sounds (script events) + the scene's audio sources,
        // heard from this rider's camera.
        const runtime::CameraView cam = follow_camera(client);
        sound.stage().update_2d(client.world(), cam.x, cam.y);
      }
      sound.tick(dt);
      for (const std::string& e : client.take_events()) std::cout << "[posse] " << e << '\n';
      if (client.state() != shown) {
        shown = client.state();
        std::cout << "[ts_client] " << mp::to_string(shown)
                  << (client.status().empty() ? "" : ": " + client.status()) << '\n';
      }
      if (client.state() == mp::ClientState::Rejected ||
          client.state() == mp::ClientState::Disconnected) {
        code = 1;
        break;
      }
      int ww = 0, wh = 0, fw = 0, fh = 0;
      glfwGetWindowSize(win, &ww, &wh);
      glfwGetFramebufferSize(win, &fw, &fh);
      if (ww > 0 && wh > 0 && fw > 0 && fh > 0) {
        const runtime::WorldRect view = runtime::World::view_rect(
            follow_camera(client), static_cast<float>(ww), static_cast<float>(wh));
        if (client.has_snapshot()) {
          client.world().build_draw_list(
              view, 1.0f,
              [&renderer](const std::string& rel, int* w, int* h) {
                return renderer.image_size(rel, w, h);
              },
              &quads);
        } else {
          quads.clear();
        }
        renderer.render(view, fw, fh, quads);
        game::render_toasts(client_toasts(client), fw, fh);
        gui.render_gl(renderer, fw, fh);
        glfwSwapBuffers(win);
      }
      if (client.clock() - title_at >= 0.5) {
        title_at = client.clock();
        const mp::ClientStats& s = client.stats();
        char title[384];
        std::snprintf(title, sizeof(title),
                      "%s | TombStone Client | %s as P%d | %zu riding | RTT %.0f ms | %.0f "
                      "snapshots/s | %.1f KB/s in",
                      client.project_name().empty() ? "riding in" : client.project_name().c_str(),
                      client.name().c_str(), client.slot() + 1, client.roster().riders.size(),
                      s.rtt_ms, s.snapshot_hz, s.kbps_in);
        glfwSetWindowTitle(win, title);
      }
      if (opt.frames >= 0 && ++frames >= opt.frames) break;
    }
    renderer.clear();
  }
  client.disconnect();
  rp.end();
  gfx::set_texture_uploader(nullptr);
  glfwDestroyWindow(win);
  glfwTerminate();
  std::cout << "[ts_client] " << client.name() << " rode off. So long.\n";
  return code;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::string err;
  if (!parse_args(argc, argv, &opt, &err)) {
    std::cerr << err << '\n' << kUsage;
    return 2;
  }
  if (opt.help) {
    std::cout << kUsage;
    return 0;
  }
  std::cout << "TombStone Client\n";
  if (opt.smoke) return run_smoke();
  if (opt.connect.empty()) {
    std::cerr << "ts_client needs a server to ride to.\n" << kUsage;
    return 2;
  }
  if (!opt.project.empty() && !fs::is_directory(opt.project)) {
    std::cerr << "No project folder at " << opt.project << '\n';
    return 2;
  }
  return run_window(opt);
}
