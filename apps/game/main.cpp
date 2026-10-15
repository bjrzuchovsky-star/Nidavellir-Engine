// ts_game: the shipped runtime. Opens a window, loads <project>/scene.json
// through runtime::World, and runs it at a fixed 60 Hz with keyboard and
// gamepad input and the scene's follow camera. No ImGui, no editor code.
//
//   ts_game --project <dir>            ride the project
//   ts_game --smoke --project <dir>    headless check (no window)
//   ts_game --project <dir> --log <f>  also keep the Telegraph in <f>
// "dimension": "3d" projects ride in a GL 3.3 core window (Game3D.cpp);
// "dimension": "vr" ones in a headset through OpenXR, else the desktop
// simulator (GameVR.cpp).
//   ts_game --xr-probe                 OpenXR runtime / headset / session check

#include "audio/GameAudio.h"
#include "Game3D.h"
#include "GameRenderer.h"
#include "GameUi.h"
#include "GameManifest.h"
#include "HudText.h"

#include "core/JsonMini.h"
#include "gfx/GlTextureUploader.h"
#include "gfx/TextureCache.h"
#include "runtime/Input.h"
#include "runtime/Log.h"
#include "plugins/Builtins.h"
#include "plugins/RuntimePlugins.h"
#include "runtime/PlaySession.h"
#include "runtime/World.h"
#include "scene/SampleScripts.h"
#include "scene/SceneJson.h"
#include "ui/UiScript.h"

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ts::tombstone;
namespace fs = std::filesystem;
using game::GameRenderer;
using game::GameUi;

constexpr const char* kUsage =
    "Usage: ts_game [--project <dir>] [--smoke] [--frames N] [--collision]\n"
    "               [--log <file>]\n"
    "  --project <dir>  TombStone project folder (holds scene.json), or an\n"
    "                   export folder (game.json + game.tspak); omitted =\n"
    "                   the executable's folder when it holds game.json\n"
    "  --smoke          headless check: load, ride 60 ticks, verify, exit\n"
    "  --frames N       quit after N rendered frames (testing)\n"
    "  --collision      start with the collision overlay on (K)\n"
    "  --log <file>     also write the Telegraph (script, toast and trigger\n"
    "                   log lines, always on stdout) to <file>\n"
    "  --sim            VR projects: use the desktop simulator, not the headset\n"
    "  --xr-probe       check the OpenXR runtime + headset (creates and destroys\n"
    "                   a session, no window) and exit\n"
    "Keys: WASD / arrows move P1 | E / Space / pad A action |\n"
    "      gamepads ride P1-P4 | P or F6 pause | F10 step while paused |\n"
    "      R or F5 reload scene.json | K collision overlay | Esc pause menu\n"
    "Scripts under <project>/scripts/ hot-reload when saved.\n";

struct Options {
  std::string project;
  bool smoke = false;
  long frames = -1;
  bool collision = false;
  bool help = false;
  bool sim = false;
  bool xr_probe = false;
  std::string log_path;
};

bool parse_args(int argc, char** argv, Options* out, std::string* err) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--project" && i + 1 < argc) {
      out->project = argv[++i];
    } else if (a == "--smoke") {
      out->smoke = true;
    } else if (a == "--frames" && i + 1 < argc) {
      out->frames = std::strtol(argv[++i], nullptr, 10);
    } else if (a == "--log" && i + 1 < argc) {
      out->log_path = argv[++i];
    } else if (a == "--sim") {
      out->sim = true;
    } else if (a == "--xr-probe") {
      out->xr_probe = true;
    } else if (a == "--collision") {
      out->collision = true;
    } else if (a == "--help" || a == "-h") {
      out->help = true;
    } else {
      *err = "Unknown or incomplete argument: " + a;
      return false;
    }
  }
  return true;
}

std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Display name from project.json, else the folder name.
std::string project_name(const std::string& dir) {
  const std::string text = read_file(fs::path(dir) / "project.json");
  if (auto obj = json_mini::parse_object(text)) {
    const std::string name = json_mini::get_string(*obj, "name", "");
    if (!name.empty()) {
      return name;
    }
  }
  const fs::path p = fs::path(dir).lexically_normal();
  const std::string leaf =
      (p.has_filename() ? p.filename() : p.parent_path().filename()).string();
  return leaf.empty() ? std::string("TombStone") : leaf;
}

// The Telegraph on the wire: every world log line (scripts, toasts,
// trigger events) goes to stdout, and to --log <file> when asked.
class Telegraph {
 public:
  bool open(const std::string& path, std::string* err) {
    if (path.empty()) {
      return true;
    }
    file_.open(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!file_) {
      *err = "could not open log file " + path;
      return false;
    }
    return true;
  }

  // Prints and forgets everything the world wrote since the last drain.
  void drain(runtime::World& world) {
    for (const runtime::LogEntry& e : world.take_logs()) {
      const std::string line = runtime::format_log(e, true);
      std::cout << "[telegraph] " << line << '\n';
      if (file_) {
        file_ << line << '\n';
      }
      ++lines_;
      errors_ += e.level == runtime::LogLevel::Error ? 1 : 0;
    }
    std::cout.flush();
    if (file_) {
      file_.flush();
    }
  }

  long lines() const { return lines_; }
  long errors() const { return errors_; }

 private:
  std::ofstream file_;
  long lines_ = 0;
  long errors_ = 0;
};

float deadzone(float v) {
  constexpr float kDead = 0.2f;
  if (std::fabs(v) < kDead) {
    return 0.0f;
  }
  const float s = (std::fabs(v) - kDead) / (1.0f - kDead);
  return v < 0.0f ? -s : s;
}

runtime::InputFrame poll_input(GLFWwindow* win) {
  runtime::InputFrame frame;
  auto down = [win](int a, int b) {
    return glfwGetKey(win, a) == GLFW_PRESS || glfwGetKey(win, b) == GLFW_PRESS;
  };
  runtime::PlayerInput keys = runtime::digital_input(
      down(GLFW_KEY_A, GLFW_KEY_LEFT), down(GLFW_KEY_D, GLFW_KEY_RIGHT),
      down(GLFW_KEY_W, GLFW_KEY_UP), down(GLFW_KEY_S, GLFW_KEY_DOWN));
  // Action (E / Space / pad A): talk to, open or pick up what is nearby.
  if (down(GLFW_KEY_E, GLFW_KEY_SPACE)) {
    keys.buttons |= runtime::kButtonAction;
  }
  if (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) {
    keys.buttons |= runtime::kButtonAlt;
  }
  if (glfwGetKey(win, GLFW_KEY_ENTER) == GLFW_PRESS) {
    keys.buttons |= runtime::kButtonStart;
  }
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
  frame.slot(0) = keys;
  // Gamepads 1-4 ride slots 0-3 (slot 0 shares with the keyboard).
  for (int slot = 0; slot < kMaxPlayers; ++slot) {
    GLFWgamepadstate pad;
    if (!glfwGetGamepadState(GLFW_JOYSTICK_1 + slot, &pad)) {
      continue;
    }
    runtime::PlayerInput in;
    in.connected = true;
    in.move_x = deadzone(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X]);
    in.move_y = deadzone(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
    if (pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT]) in.move_x = -1.0f;
    if (pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT]) in.move_x = 1.0f;
    if (pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP]) in.move_y = -1.0f;
    if (pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN]) in.move_y = 1.0f;
    if (pad.buttons[GLFW_GAMEPAD_BUTTON_A]) in.buttons |= runtime::kButtonAction;
    if (pad.buttons[GLFW_GAMEPAD_BUTTON_B]) in.buttons |= runtime::kButtonAlt;
    if (pad.buttons[GLFW_GAMEPAD_BUTTON_START]) {
      in.buttons |= runtime::kButtonStart;
    }
    frame.slot(slot) = runtime::merge_input(frame.slot(slot), in);
  }
  return frame;
}

// The project's plugins (project.json "plugins"; runtime built-ins + DLLs),
// reported on stdout like the Telegraph.
void open_plugins(plugins::RuntimePlugins& rp, const std::string& dir) {
  const std::size_t n = rp.open(dir, "game", [](const plugin::Diagnostic& d) {
    std::cout << "[plugins] [" << runtime::to_string(d.level) << "] "
              << (d.plugin.empty() ? "" : d.plugin + ": ") << d.text << '\n';
  });
  std::cout << "[ts_game] " << n << " plugin(s) riding\n";
}

// --smoke, part two: when the project still has the seeded corral gate and
// gold nugget (scripts/gate.lua, scripts/gold.lua), ride them headless on a
// fresh session: left onto the nugget, on to the gate, action button, and
// through. Their Telegraph lines print as they happen. "" = all good.
std::string ride_sample_scripts(const std::string& dir, Telegraph& telegraph,
                                std::string* note) {
  runtime::PlaySession session;
  std::string err;
  if (!session.start_project(dir, &err)) {
    return err;
  }
  runtime::World& world = session.world();
  std::uint64_t gate = 0;
  std::uint64_t gold = 0;
  for (const runtime::Actor& a : world.actors()) {
    if (!a.data.script) continue;
    if (a.data.script->path == sample_scripts::kGate) gate = a.data.id;
    if (a.data.script->path == sample_scripts::kGold) gold = a.data.id;
  }
  const runtime::Actor* rider = world.player(0);
  if (!gate || !gold || !rider) {
    *note = "no sample gate / nugget in this scene";
    return "";
  }
  const std::uint64_t rider_id = rider->data.id;
  runtime::InputFrame left;
  left.slot(0).move_x = -1.0f;
  left.slot(0).connected = true;
  runtime::InputFrame action = left;
  action.slot(0).buttons = runtime::kButtonAction;
  bool picked = false;
  bool opened = false;
  bool toasted = false;
  auto tick = [&](const runtime::InputFrame& in) {
    session.run_ticks(1, in);
    for (const runtime::LogEntry& e : world.pending_logs()) {
      picked |= e.file == sample_scripts::kGold &&
                e.text.find("picked up") != std::string::npos;
      opened |= e.file == sample_scripts::kGate &&
                e.text.find("opened by P1") != std::string::npos;
      toasted |= e.channel == "toast";
      if (e.level == runtime::LogLevel::Error) {
        err = runtime::format_log(e);
      }
    }
    telegraph.drain(world);
  };
  // Left onto the nugget: pocketed, toasted, gone.
  std::uint64_t gold_tick = 0;
  for (int i = 0; i < 120 && world.find(gold) && err.empty(); ++i) {
    tick(left);
    gold_tick = session.tick();
  }
  const runtime::Actor* r = world.find(rider_id);
  if (!err.empty()) return err;
  if (world.find(gold) || !picked || !toasted || !r ||
      r->state.count("gold") == 0 || r->state.at("gold").number != 10.0) {
    return "the gold nugget should be pocketed (10 gold, toast, gone)";
  }
  // On to the gate: a solid wall until the action button.
  const runtime::Actor* g = world.find(gate);
  const float gate_right = g ? g->data.x + g->data.w : 0.0f;
  for (int i = 0; i < 90; ++i) tick(left);
  r = world.find(rider_id);
  if (!r || std::fabs(r->data.x - gate_right) > 0.05f) {
    return "the shut gate should stop the rider";
  }
  if (world.interact_target(rider_id) != gate) {
    return "the gate should be the rider's action target";
  }
  tick(action);
  const std::uint64_t gate_tick = session.tick();
  g = world.find(gate);
  if (!err.empty()) return err;
  if (!opened || !g || !g->hidden || g->data.collider) {
    return "the action button should swing the gate open";
  }
  for (int i = 0; i < 30; ++i) tick(left);
  r = world.find(rider_id);
  if (!err.empty()) return err;
  if (!r || r->data.x >= g->data.x) {
    return "the rider should ride through the open gate";
  }
  session.stop();
  *note = "gold pocketed at tick " + std::to_string(gold_tick) +
          ", gate opened by the action button at tick " +
          std::to_string(gate_tick) + ", rode through";
  return "";
}

// --smoke: everything the window does except the window.
int run_smoke(const std::string& dir, Telegraph& telegraph) {
  auto fail = [](const std::string& what) {
    std::cerr << "[smoke] ts_game FAILED: " << what << '\n';
    return 1;
  };
  const fs::path scene = scene_json::scene_path_for_project(dir);
  if (!fs::exists(scene)) {
    return fail("no scene.json in " + dir);
  }
  const std::string disk_before = read_file(scene);

  plugins::RuntimePlugins rp;
  open_plugins(rp, dir);
  runtime::PlaySession session;
  rp.attach(session.world());
  std::string err;
  if (!session.start_project(dir, &err)) {
    return fail(err);
  }
  rp.begin(session.world());
  runtime::World& world = session.world();
  world.set_view_size(1280.0f, 720.0f);
  const std::size_t actor_count = world.actors().size();
  const runtime::CameraView cam0 = world.camera();
  const runtime::Actor* rider = world.player(0);
  const float x0 = rider ? rider->data.x : 0.0f;
  const float y0 = rider ? rider->data.y : 0.0f;

  runtime::InputFrame right;
  right.slot(0).move_x = 1.0f;
  right.slot(0).connected = true;
  // Tick by tick, counting P1's animation frame changes within one clip.
  int frame_changes = 0;
  for (int i = 0; i < runtime::kTickRate; ++i) {
    const runtime::Actor* a = world.player(0);
    const std::string clip0 = a ? a->anim.clip : std::string();
    const int frame0 = a ? a->anim.frame : 0;
    session.run_ticks(1, right);
    telegraph.drain(world);
    a = world.player(0);
    if (a && !clip0.empty() && a->anim.clip == clip0 && a->anim.frame != frame0) {
      ++frame_changes;
    }
  }
  if (session.tick() != static_cast<std::uint64_t>(runtime::kTickRate)) {
    return fail("expected 60 ticks");
  }

  std::string ride = "no rider (add a Player component)";
  // Airship Works flies P1 (throttle / strafe), so the walk check does not
  // apply; the ship must still exist and the ride still tick.
  bool flies = false;
  for (const std::string& id : plugins::read_project_plugins(dir).ids) {
    flies = flies || id == "ts.shipyard";
  }
  if (rider && flies) {
    rider = world.player(0);
    ride = "P1 aboard an airship (flight controls; walk check skipped)";
  } else if (rider) {
    rider = world.player(0);
    const float want = x0 + rider->data.player->speed;
    if (std::fabs(rider->data.x - want) > 0.05f ||
        std::fabs(rider->data.y - y0) > 0.001f) {
      return fail("P1 x " + std::to_string(rider->data.x) + " want " +
                  std::to_string(want));
    }
    const runtime::CameraView cam1 = world.camera();
    const bool follows = world.camera_target() == rider->data.id;
    // A bounded camera may legitimately sit against its fence.
    const runtime::Actor* cam_actor = world.find(world.camera_entity());
    const bool bounded = cam_actor && cam_actor->data.camera &&
                         cam_actor->data.camera->use_bounds;
    const bool moved = cam1.x > cam0.x;
    if (follows && !moved && !bounded) {
      return fail("camera did not follow P1");
    }
    const char* cam_note = !follows ? "follows someone else"
                           : moved  ? "followed"
                                    : "held at its bounds";
    char buf[160];
    std::snprintf(buf, sizeof(buf), "P1 rode %.0f px in 60 ticks, camera %s",
                  static_cast<double>(rider->data.x - x0), cam_note);
    ride = buf;
  }

  // Draw list with real image sizes (decoded headless; no GL uploader).
  GameRenderer renderer(dir);
  std::vector<runtime::DrawQuad> quads;
  const runtime::CameraView cam = world.camera(session.alpha());
  world.build_draw_list(
      runtime::World::view_rect(cam, 1280.0f, 720.0f), session.alpha(),
      [&renderer](const std::string& rel, int* w, int* h) {
        return renderer.image_size(rel, w, h);
      },
      &quads);
  if (quads.empty()) {
    return fail("nothing to draw around the camera");
  }
  // An animated P1 draws a frame of its sheet and changed frames on the ride.
  std::string anim_note = "P1 not animated";
  const AnimLibrary::Entry* anim = nullptr;
  if (rider && (anim = world.anim_set(*rider)) != nullptr) {
    if (!anim->ok) {
      anim_note = "P1 animator: " + anim->error;
    } else {
      const runtime::DrawQuad* rq = nullptr;
      for (const runtime::DrawQuad& q : quads) {
        if (q.entity == rider->data.id) rq = &q;
      }
      if (!rq || rq->missing || !rq->image || *rq->image != anim->image) {
        return fail("P1 should draw a frame of " + anim->image);
      }
      if (frame_changes < 2) {
        return fail("P1 animation did not advance (" +
                    std::to_string(frame_changes) + " frame changes)");
      }
      anim_note = "P1 " + rider->anim.clip + " " +
                  std::to_string(frame_changes) + " frame changes";
    }
  }

  // K overlay over the same view: P1's collider is there, solid tiles
  // come from the project's tile_solidity.
  std::vector<runtime::OverlayBox> boxes;
  world.build_overlay(runtime::World::view_rect(cam, 1280.0f, 720.0f),
                      session.alpha(), &boxes);
  int solid_tiles = 0;
  bool rider_box = false;
  for (const runtime::OverlayBox& b : boxes) {
    solid_tiles += b.kind == runtime::OverlayKind::SolidTile ? 1 : 0;
    rider_box |= rider && b.entity == rider->data.id;
  }
  if (rider && rider->data.collider && !rider_box) {
    return fail("collision overlay is missing P1's collider");
  }
  const std::uint64_t trigger_events = world.trigger_event_total();

  // Pause holds; a step is one tick.
  session.pause();
  const std::uint64_t t = session.tick();
  if (session.update(1.0, right) != 0 || !session.step_once(right) ||
      session.tick() != t + 1) {
    return fail("pause / step");
  }
  telegraph.drain(world);
  session.stop();
  std::string scripted;
  const std::string script_err = ride_sample_scripts(dir, telegraph, &scripted);
  if (!script_err.empty()) {
    return fail("sample scripts: " + script_err);
  }
  if (read_file(scene) != disk_before) {
    return fail("scene.json changed (the game must never write it)");
  }
  std::cout << "[smoke] ts_game OK (\"" << project_name(dir) << "\": "
            << actor_count << " actors, " << ride << ", " << anim_note << ", "
            << quads.size() << " quads, K overlay " << boxes.size()
            << (boxes.size() == 1 ? " box" : " boxes") << " ("
            << solid_tiles << " solid tile run" << (solid_tiles == 1 ? "" : "s")
            << "), " << trigger_events << " trigger event"
            << (trigger_events == 1 ? "" : "s") << ", " << telegraph.lines()
            << " telegraph line" << (telegraph.lines() == 1 ? "" : "s")
            << ", sample scripts: " << scripted << ", scene.json untouched)\n";
  return 0;
}

struct GameState {
  // Declared before the session: destroyed after it (the world holds the
  // plugins' systems).
  plugins::RuntimePlugins plugins;
  runtime::PlaySession session;
  bool reload = false;
  bool show_collision = false;
  GameUi* ui = nullptr;  // menus / text boxes take the keys while open
};

void on_key(GLFWwindow* win, int key, int /*scancode*/, int action,
            int mods) {
  auto* g = static_cast<GameState*>(glfwGetWindowUserPointer(win));
  if (!g || action == GLFW_RELEASE) {
    return;
  }
  const bool repeat = action == GLFW_REPEAT;
  // Esc (pause menu) and menu keys are polled by GameUi.
  if (g->ui && (g->ui->wants_input() || key == GLFW_KEY_ESCAPE)) {
    return;
  }
  if ((key == GLFW_KEY_P || key == GLFW_KEY_F6) && !repeat) {
    g->session.toggle_pause();
  } else if (key == GLFW_KEY_F10) {
    g->session.step_once(poll_input(win));
  } else if ((key == GLFW_KEY_R || key == GLFW_KEY_F5) && !repeat &&
             (mods & GLFW_MOD_CONTROL) == 0) {
    g->reload = true;
  } else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9 && !repeat) {
    // Hotbar slots 1..9, 0 = the tenth (Frontier Survival).
    plugins::select_hotbar(g->session.world(), 0, key == GLFW_KEY_0 ? 9 : key - GLFW_KEY_1);
  } else if (key == GLFW_KEY_K && !repeat) {
    g->show_collision = !g->show_collision;
    std::cout << "[ts_game] collision overlay "
              << (g->show_collision ? "on" : "off") << '\n';
  }
}

void on_glfw_error(int code, const char* message) {
  std::cerr << "[ts_game] GLFW error " << code << ": "
            << (message ? message : "") << '\n';
}

int run_window(const Options& opt, Telegraph& telegraph) {
  const std::string name = project_name(opt.project);
  glfwSetErrorCallback(on_glfw_error);
  if (!glfwInit()) {
    std::cerr << "[ts_game] could not start GLFW\n";
    return 1;
  }
  // Default (compatibility) context: the renderer is fixed-function GL.
  GLFWwindow* win = glfwCreateWindow(game::window_width(), game::window_height(),
                                     (name + " | TombStone").c_str(),
                                     nullptr, nullptr);
  if (!win) {
    std::cerr << "[ts_game] could not open a window\n";
    glfwTerminate();
    return 1;
  }
  game::apply_window_icon(win, opt.project);
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);

  gfx::GlTextureUploader uploader;
  gfx::set_texture_uploader(&uploader);
  int code = 0;
  {
    GameRenderer renderer(opt.project);
    GameState g;
    g.show_collision = opt.collision;
    open_plugins(g.plugins, opt.project);
    g.plugins.attach(g.session.world());
    std::string err;
    const bool started = g.session.start_project(opt.project, &err);
    if (started) g.plugins.begin(g.session.world());
    if (!started) {
      std::cerr << "[ts_game] " << err << '\n';
      code = 1;
    } else {
      GameUi gui;
      gui.load(opt.project);
      gui.attach(win);
      g.ui = &gui;
      bool menu_paused = false;  // the pause menu holds the session
      glfwSetWindowUserPointer(win, &g);
      glfwSetKeyCallback(win, on_key);
      std::cout << "[ts_game] riding \"" << name << "\": "
                << g.session.world().actors().size() << " actors, "
                << g.session.world().player_count() << " rider(s)\n";

      std::vector<runtime::DrawQuad> quads;
      std::vector<runtime::OverlayBox> boxes;
      ts::audio::GameAudio sound(opt.project, false, "ts_game");
      double last = glfwGetTime();
      double title_at = last;
      double scripts_at = last;
      long frames = 0;
      while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        if (g.reload) {
          g.reload = false;
          telegraph.drain(g.session.world());
          renderer.clear();
          g.plugins.end();
          if (g.session.start_project(opt.project, &err)) {
            g.plugins.begin(g.session.world());
            sound.restart(opt.project, false);
            gui.load(opt.project);
            std::cout << "[ts_game] reloaded scene.json\n";
          } else {
            std::cerr << "[ts_game] reload failed: " << err << '\n';
            break;
          }
        }
        const double now = glfwGetTime();
        {
          // In-game UI: input, pause menu, Lua ui.* both ways.
          const GameUi::Frame uf = gui.update(win, now - last, &sound.engine());
          if (uf.quit) glfwSetWindowShouldClose(win, GLFW_TRUE);
          if (gui.pause_open() && !g.session.paused()) {
            g.session.toggle_pause();
            menu_paused = true;
          } else if (!gui.pause_open() && menu_paused) {
            if (g.session.paused()) g.session.toggle_pause();
            menu_paused = false;
          }
          ui::forward_ui_events(g.session.world(), uf.events);
        }
        g.session.update(now - last,
                         gui.wants_input() ? runtime::InputFrame{} : poll_input(win));
        ui::apply_ui_commands(gui.system(), g.session.world().take_ui_commands());
        {
          // Ears on the camera centre.
          runtime::World& world = g.session.world();
          const runtime::CameraView cam = world.camera(g.session.alpha());
          sound.stage().update_2d(world, cam.x, cam.y);
          sound.tick(now - last);
        }
        last = now;
        renderer.poll_changes(now);
        // Saved scripts ride back in without a restart (a broken save keeps
        // the old version running and says why on the Telegraph).
        if (now - scripts_at >= 0.5) {
          scripts_at = now;
          g.session.world().reload_scripts();
        }
        telegraph.drain(g.session.world());

        int ww = 0, wh = 0, fw = 0, fh = 0;
        glfwGetWindowSize(win, &ww, &wh);
        glfwGetFramebufferSize(win, &fw, &fh);
        if (ww > 0 && wh > 0 && fw > 0 && fh > 0) {
          runtime::World& world = g.session.world();
          world.set_view_size(static_cast<float>(ww), static_cast<float>(wh));
          const float alpha = g.session.alpha();
          const runtime::WorldRect view = runtime::World::view_rect(
              world.camera(alpha), static_cast<float>(ww),
              static_cast<float>(wh));
          world.build_draw_list(
              view, alpha,
              [&renderer](const std::string& rel, int* w, int* h) {
                return renderer.image_size(rel, w, h);
              },
              &quads);
          renderer.render(view, fw, fh, quads);
          if (g.show_collision) {
            world.build_overlay(view, alpha, &boxes);
            renderer.render_overlay(boxes);
          }
          game::render_toasts(game::hud_toasts(world), fw, fh);
          gui.render_gl(renderer, fw, fh);
          glfwSwapBuffers(win);
        }
        if (now - title_at >= 0.5) {
          title_at = now;
          char title[384];
          const runtime::World& world = g.session.world();
          const runtime::Actor* p1 = world.player(0);
          // Last trigger event rides along in the title bar.
          std::string event;
          if (const auto& ev = world.last_trigger_event()) {
            event = " | " + world.describe(*ev) + " (tick " +
                    std::to_string(ev->tick) + ")";
          }
          std::snprintf(title, sizeof(title),
                        "%s | TombStone | %s | %.0f ticks/s | P1 %.0f, %.0f%s%s",
                        name.c_str(),
                        g.session.paused() ? "HOLDING UP (P)" : "riding",
                        g.session.ticks_per_second(),
                        p1 ? static_cast<double>(p1->data.x) : 0.0,
                        p1 ? static_cast<double>(p1->data.y) : 0.0,
                        g.show_collision ? " | collision (K)" : "",
                        event.c_str());
          glfwSetWindowTitle(win, title);
        }
        if (opt.frames >= 0 && ++frames >= opt.frames) {
          break;
        }
      }
      telegraph.drain(g.session.world());
      std::cout << "[ts_game] rode " << g.session.tick() << " ticks";
      if (telegraph.errors() > 0) {
        std::cout << " (" << telegraph.errors() << " script error"
                  << (telegraph.errors() == 1 ? "" : "s") << " on the Telegraph)";
      }
      std::cout << ". So long.\n";
      glfwSetWindowUserPointer(win, nullptr);
      gui.detach(win);
    }
    renderer.clear();  // drop GL textures while the context lives
  }
  gfx::set_texture_uploader(nullptr);
  glfwDestroyWindow(win);
  glfwTerminate();
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
  if (opt.xr_probe) {
    return game::run_xr_probe();
  }
  // Exported games: game.json next to the exe (or in --project) mounts
  // the .tspak through ts_vfs and hands its folder to the loaders.
  if (!game::resolve_launch(argc > 0 ? argv[0] : nullptr, &opt.project, &err)) {
    std::cerr << err << '\n' << kUsage;
    return 2;
  }
  if (!fs::is_directory(opt.project)) {
    std::cerr << "No project folder at " << opt.project << '\n';
    return 2;
  }
  Telegraph telegraph;
  if (!telegraph.open(opt.log_path, &err)) {
    std::cerr << err << '\n';
    return 2;
  }
  std::cout << "TombStone Game\n";
  if (game::is_vr_project(opt.project)) {
    const std::string name = project_name(opt.project);
    return opt.smoke ? game::run_smoke_vr(opt.project, name)
                     : game::run_window_vr(opt.project, name, opt.frames, opt.sim);
  }
  if (game::is_3d_project(opt.project)) {
    const std::string name = project_name(opt.project);
    return opt.smoke ? game::run_smoke_3d(opt.project, name)
                     : game::run_window_3d(opt.project, name, opt.frames);
  }
  return opt.smoke ? run_smoke(opt.project, telegraph)
                   : run_window(opt, telegraph);
}
