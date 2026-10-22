// ts_admin --smoke-mouselook (also part of --smoke-templates): mouse look
// on the built-in player movement, driven by synthetic mouse deltas (no
// window, no cursor).
//   3D first person: yaw on the body, pitch on the eyes, the +-89 deg pitch
//     clamp, invert Y, sensitivity, smoothing, once-per-frame deltas,
//     walking where you look, Keys look ignoring the mouse, the right stick.
//   3D third person: the orbit (pitch lifts / drops the camera, clamped),
//     scroll zoom, the camera pulled in front of a wall, the rider turning
//     to face where it walks.
//   2D top-down: the face-mouse angle and the drawn rotation (collider
//     untouched); platformer riders ignore it.
//   scene.json round trips, and the Empty templates ship with it on.
#include "SmokeMouseLook.h"

#include "editor/templates/EmptyTemplates.h"
#include "render3d/Scene3D.h"
#include "runtime/Input.h"
#include "runtime/World.h"
#include "scene/SceneJson.h"
#include "world3d/World3D.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace ts {
namespace tombstone {

namespace {

namespace r3 = render3d;
namespace s3 = scene3d;
namespace w3 = world3d;

int fail(const std::string& why) {
  std::cerr << "[smoke] mouse look FAILED: " << why << '\n';
  return 1;
}

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

std::string f2(float v) {
  char b[32];
  std::snprintf(b, sizeof(b), "%.2f", v);
  return b;
}

s3::Entity3D ground() {
  s3::Entity3D g;
  g.id = 100;
  g.name = "Ground";
  g.transform.position = {0.0f, -0.5f, 0.0f};
  s3::Collider3D c;
  c.size = {60.0f, 1.0f, 60.0f};
  g.collider = c;
  return g;
}

s3::Entity3D rider(bool first_person) {
  s3::Entity3D p;
  p.id = 1;
  p.name = "Player";
  p.transform.position = {0.0f, 1.0f, 0.0f};
  s3::Collider3D c;
  c.shape = s3::Collider3D::Shape::Capsule;
  c.size = {0.4f, 2.0f, 0.4f};
  p.collider = c;
  s3::Player3D pl;
  if (first_person) pl.view = s3::Player3D::View::FirstPerson;
  p.player = pl;
  return p;
}

bool ride(w3::World3D& w, const s3::Entity3D& player, std::vector<s3::Entity3D> extra,
          std::string* why) {
  s3::Scene3D s;
  s.entities.push_back(ground());
  s.entities.push_back(player);
  for (auto& e : extra) s.entities.push_back(std::move(e));
  s.editor_camera.yaw = 0.0f;
  if (!w.start(s, "", why)) return false;
  w.run_ticks(20, {});  // settle on the ground
  return true;
}

w3::Input3D mouse(float dx, float dy) {
  w3::Input3D in;
  in.mouse_dx = dx;
  in.mouse_dy = dy;
  return in;
}

r3::Vec3 facing(const s3::Entity3D& e) { return e.transform.rotation.rotate({0.0f, 0.0f, -1.0f}); }

int first_person(std::string* note) {
  std::string err;
  w3::World3D w;
  if (!ride(w, rider(true), {}, &err)) return fail("FPS start: " + err);
  if (!w.mouse_look() || !w.wants_cursor_capture()) return fail("FPS: mouse look + capture by default");
  const float yaw0 = w.camera_yaw();
  // 100 counts right at 0.15 deg / count: 15 deg to the right (yaw down).
  w.step(mouse(100.0f, 0.0f));
  if (!near(w.camera_yaw(), yaw0 - 15.0f)) {
    return fail("FPS yaw: 100 counts right turned " + f2(w.camera_yaw() - yaw0) + " deg");
  }
  // The body faces the view.
  const r3::Vec3 f = facing(*w.find(w.player_id()));
  const float yr = r3::radians(w.camera_yaw());
  if (std::fabs(f.x + std::sin(yr)) > 0.02f || std::fabs(f.z + std::cos(yr)) > 0.02f) {
    return fail("FPS: the body turns with the yaw");
  }
  // Up looks up, pitch on the eyes.
  w.step(mouse(0.0f, -100.0f));
  if (!near(w.camera_pitch(), 15.0f)) return fail("FPS pitch: 100 counts up gave " + f2(w.camera_pitch()));
  const r3::Vec3 look = w.camera().target - w.camera().position;
  if (look.y <= 0.2f) return fail("FPS: the eyes tilt up");
  // Clamp.
  w.step(mouse(0.0f, -100000.0f));
  if (!near(w.camera_pitch(), w3::World3D::kFirstPersonPitchLimit)) {
    return fail("FPS pitch clamp up: " + f2(w.camera_pitch()));
  }
  w.step(mouse(0.0f, 100000.0f));
  if (!near(w.camera_pitch(), -w3::World3D::kFirstPersonPitchLimit)) {
    return fail("FPS pitch clamp down: " + f2(w.camera_pitch()));
  }
  w.step(mouse(0.0f, -w3::World3D::kFirstPersonPitchLimit / 0.15f));  // back to level
  // update() applies a frame's deltas once, however many ticks run.
  const float yaw1 = w.camera_yaw();
  w.update(4.0 * w3::World3D::kTickSeconds + 1e-6, mouse(-100.0f, 0.0f));
  if (!near(w.camera_yaw(), yaw1 + 15.0f)) {
    return fail("FPS: update() applies the mouse once per frame (" + f2(w.camera_yaw() - yaw1) + ")");
  }
  // Walk where you look: turn 90 deg left, then forward.
  w.step(mouse(-600.0f, 0.0f));
  const float yaw2 = r3::radians(w.camera_yaw());
  const r3::Vec3 a = w.find(w.player_id())->transform.position;
  w3::Input3D fwd;
  fwd.move_forward = 1.0f;
  w.run_ticks(40, fwd);
  const r3::Vec3 b = w.find(w.player_id())->transform.position;
  r3::Vec3 d{b.x - a.x, 0.0f, b.z - a.z};
  const float dist = r3::length(d);
  d = r3::normalize(d);
  if (dist < 1.0f || d.x * -std::sin(yaw2) + d.z * -std::cos(yaw2) < 0.95f) {
    return fail("FPS: W walks where you look");
  }
  // Right stick: a second at full right turns kStickLookSpeed.
  const float yaw3 = w.camera_yaw();
  w3::Input3D stick;
  stick.stick_x = 1.0f;
  w.run_ticks(60, stick);
  if (!near(w.camera_yaw(), yaw3 - w3::World3D::kStickLookSpeed, 0.1f)) {
    return fail("FPS: the right stick turns (" + f2(w.camera_yaw() - yaw3) + ")");
  }
  w.stop();

  // Invert Y + sensitivity.
  s3::Entity3D inv = rider(true);
  inv.player->invert_y = true;
  inv.player->mouse_sensitivity = 0.3f;
  w3::World3D wi;
  if (!ride(wi, inv, {}, &err)) return fail("FPS invert start: " + err);
  wi.step(mouse(0.0f, -50.0f));
  if (!near(wi.camera_pitch(), -15.0f)) return fail("invert Y: up looks down (" + f2(wi.camera_pitch()) + ")");
  wi.stop();

  // Smoothing eases toward the target.
  s3::Entity3D sm = rider(true);
  sm.player->smoothing = 0.5f;
  w3::World3D ws;
  if (!ride(ws, sm, {}, &err)) return fail("smoothing start: " + err);
  const float y0 = ws.camera_yaw();
  ws.step(mouse(200.0f, 0.0f));  // target -30
  const float part = y0 - ws.camera_yaw();
  ws.run_ticks(40, {});
  if (!(part > 1.0f && part < 29.0f) || !near(ws.camera_yaw(), y0 - 30.0f, 0.05f)) {
    return fail("smoothing: first tick " + f2(part) + " deg, settles at " + f2(y0 - ws.camera_yaw()));
  }
  ws.stop();

  // Keys look: the mouse does nothing, Q / Z still turn, no capture.
  s3::Entity3D keys = rider(true);
  keys.player->look = s3::Player3D::Look::Keys;
  w3::World3D wk;
  if (!ride(wk, keys, {}, &err)) return fail("keys start: " + err);
  const float k0 = wk.camera_yaw();
  wk.step(mouse(300.0f, -300.0f));
  if (wk.mouse_look() || wk.wants_cursor_capture() || !near(wk.camera_yaw(), k0) ||
      !near(wk.camera_pitch(), 0.0f)) {
    return fail("Keys look: the mouse is ignored and the cursor left alone");
  }
  w3::Input3D q;
  q.turn = 2.0f;
  wk.run_ticks(10, q);
  if (!near(wk.camera_yaw(), k0 + 20.0f)) return fail("Keys look: Q / Z turn");
  wk.stop();

  // Capture off: still mouse look, cursor free.
  s3::Entity3D free = rider(true);
  free.player->capture_cursor = false;
  w3::World3D wf;
  if (!ride(wf, free, {}, &err)) return fail("capture-off start: " + err);
  if (!wf.mouse_look() || wf.wants_cursor_capture()) return fail("capture off: mouse look, no capture");
  wf.stop();
  *note += " FPS yaw/pitch/clamp/invert/smoothing/stick ok;";
  return 0;
}

int third_person(std::string* note) {
  std::string err;
  w3::World3D w;
  if (!ride(w, rider(false), {}, &err)) return fail("TPS start: " + err);
  const r3::Vec3 pos = w.find(w.player_id())->transform.position;
  w.run_ticks(30, {});
  const float level_y = w.camera().position.y;
  // Mouse down looks down: the camera rises above the rider.
  w.step(mouse(0.0f, 200.0f));
  if (!near(w.camera_pitch(), -30.0f)) return fail("TPS pitch: " + f2(w.camera_pitch()));
  w.run_ticks(60, {});
  if (w.camera().position.y < level_y + 1.5f) return fail("TPS: looking down lifts the orbit camera");
  // Clamps.
  w.step(mouse(0.0f, 100000.0f));
  if (!near(w.camera_pitch(), w3::World3D::kThirdPersonPitchMin)) return fail("TPS pitch clamp down");
  w.step(mouse(0.0f, -100000.0f));
  if (!near(w.camera_pitch(), w3::World3D::kThirdPersonPitchMax)) return fail("TPS pitch clamp up");
  w.step(mouse(0.0f, w3::World3D::kThirdPersonPitchMax / 0.15f));  // level
  // Orbit yaw: 90 deg round, the camera goes with it.
  w.step(mouse(-600.0f, 0.0f));
  w.run_ticks(90, {});
  const float yr = r3::radians(w.camera_yaw());
  const r3::Vec3 c = w.camera().position;
  r3::Vec3 off = r3::normalize(r3::Vec3{c.x - pos.x, 0.0f, c.z - pos.z});
  if (off.x * std::sin(yr) + off.z * std::cos(yr) < 0.95f) return fail("TPS: the mouse orbits the camera");
  // Zoom in.
  const float d0 = w.camera_distance();
  w3::Input3D zoom;
  zoom.zoom = 3.0f;
  w.step(zoom);
  if (!(w.camera_distance() < d0 * 0.75f)) return fail("TPS: scroll zooms in");
  // Walk forward: relative to the camera, and the rider turns to face it.
  w3::Input3D fwd;
  fwd.move_forward = 1.0f;
  const r3::Vec3 a = w.find(w.player_id())->transform.position;
  w.run_ticks(40, fwd);
  const s3::Entity3D* p = w.find(w.player_id());
  r3::Vec3 d = r3::normalize(r3::Vec3{p->transform.position.x - a.x, 0.0f, p->transform.position.z - a.z});
  const r3::Vec3 f = facing(*p);
  if (d.x * -std::sin(yr) + d.z * -std::cos(yr) < 0.95f || f.x * d.x + f.z * d.z < 0.95f) {
    return fail("TPS: walks away from the camera and faces where it walks");
  }
  w.stop();

  // Camera collision: a wall between the rider and the camera pulls it in.
  s3::Entity3D wall;
  wall.id = 50;
  wall.name = "Wall";
  wall.transform.position = {0.0f, 2.0f, 3.0f};
  s3::Collider3D wc;
  wc.size = {8.0f, 6.0f, 0.4f};
  wall.collider = wc;
  w3::World3D ww;
  if (!ride(ww, rider(false), {wall}, &err)) return fail("TPS wall start: " + err);
  ww.run_ticks(30, {});
  const r3::Vec3 rp = ww.find(ww.player_id())->transform.position;
  const float z = ww.camera().position.z - rp.z;
  if (z > 2.9f || z < 0.3f) return fail("TPS: the camera is pulled in front of the wall (z " + f2(z) + ")");
  ww.stop();
  *note += " TPS orbit/clamp/zoom/face/wall pull-in ok;";
  return 0;
}

int top_down(std::string* note) {
  // Angles: art faces up, clockwise on screen.
  if (!near(runtime::top_down_aim_degrees(0.0f, -1.0f), 0.0f) ||
      !near(runtime::top_down_aim_degrees(1.0f, 0.0f), 90.0f) ||
      !near(runtime::top_down_aim_degrees(0.0f, 1.0f), 180.0f) ||
      !near(runtime::top_down_aim_degrees(-1.0f, 0.0f), 270.0f) ||
      !near(runtime::top_down_aim_degrees(1.0f, 1.0f), 135.0f)) {
    return fail("top-down aim angles");
  }
  Entity2D p;
  p.id = 1;
  p.name = "Player";
  p.x = 100.0f;
  p.y = 100.0f;
  p.w = 24.0f;
  p.h = 40.0f;
  p.player = PlayerControllerData{};
  p.player->face_mouse = true;
  p.collider = default_collider(p);
  Entity2D plat = p;
  plat.id = 2;
  plat.x = 400.0f;
  plat.player->slot = 1;
  plat.player->mode = PlayerControllerData::Mode::Platformer;
  runtime::World w;
  std::string err;
  if (!w.build({p, plat}, "", &err)) return fail("2D build: " + err);
  runtime::InputFrame in;
  // Cursor to the right of the rider's centre (112, 120), via a 200 x 100
  // view centred on (150, 120) at zoom 1: screen (112, 50) = world (162, 120).
  runtime::aim_at_screen(&in.slot(0), runtime::CameraView{150.0f, 120.0f, 1.0f}, 200.0f, 100.0f,
                         112.0f, 50.0f);
  if (!in.slot(0).aim || !near(in.slot(0).aim_x, 162.0f) || !near(in.slot(0).aim_y, 120.0f)) {
    return fail("2D: screen to world aim");
  }
  in.slot(1).aim = true;
  in.slot(1).aim_x = 0.0f;
  in.slot(1).aim_y = 0.0f;
  w.step(in);
  const runtime::Actor* a = w.find(1);
  if (!a->aiming || !near(a->aim_deg, 90.0f, 0.5f)) return fail("2D: faces the cursor on the right");
  // Down-left of the rider.
  in.slot(0).aim_x = a->data.x + 12.0f - 50.0f;
  in.slot(0).aim_y = a->data.y + 20.0f + 50.0f;
  w.step(in);
  a = w.find(1);
  if (!near(a->aim_deg, 225.0f, 0.5f)) return fail("2D: faces down-left (" + f2(a->aim_deg) + ")");
  if (a->data.rotation != 0.0f) return fail("2D: the collider rotation stays authored");
  std::vector<runtime::DrawQuad> quads;
  w.build_draw_list(runtime::WorldRect{-1000, -1000, 2000, 2000}, 1.0f,
                    [](const std::string&, int*, int*) { return false; }, &quads);
  bool drawn = false;
  for (const runtime::DrawQuad& q : quads) drawn = drawn || (q.entity == 1 && near(q.rotation, 225.0f, 0.5f));
  if (!drawn) return fail("2D: the sprite draws turned toward the cursor");
  // No cursor this tick: keep the heading.
  w.step(runtime::InputFrame{});
  if (!near(w.find(1)->aim_deg, 225.0f, 0.5f)) return fail("2D: keeps facing without a cursor");
  if (w.find(2)->aiming) return fail("2D: platformer riders ignore face mouse");
  // Right stick aim.
  runtime::PlayerInput pad;
  if (!runtime::aim_along_stick(&pad, w, 0, 0.0f, -1.0f) || runtime::aim_along_stick(&pad, w, 0, 0.1f, 0.0f)) {
    return fail("2D: stick aim (and its dead zone)");
  }
  runtime::InputFrame pin;
  pin.slot(0) = pad;
  w.step(pin);
  if (!near(w.find(1)->aim_deg, 0.0f, 0.5f) && !near(w.find(1)->aim_deg, 360.0f, 0.5f)) {
    return fail("2D: the right stick aims (" + f2(w.find(1)->aim_deg) + ")");
  }
  *note += " top-down facing ok;";
  return 0;
}

int round_trips(std::string* note) {
  // 3D: defaults write nothing new; every setting survives.
  s3::Entity3D e = rider(true);
  json::Value v = s3::entity_to_json(e);
  if (v["player"].has("look") || v["player"].has("invert_y") || v["player"].has("capture_cursor")) {
    return fail("3D JSON: default look settings stay out of scene.json");
  }
  e.player->look = s3::Player3D::Look::Keys;
  e.player->mouse_sensitivity = 0.4f;
  e.player->invert_y = true;
  e.player->smoothing = 0.3f;
  e.player->capture_cursor = false;
  s3::Entity3D back;
  std::string err;
  if (!s3::entity_from_json(s3::entity_to_json(e), &back, &err) || !back.player ||
      !(*back.player == *e.player)) {
    return fail("3D JSON: look settings round trip " + err);
  }
  // 2D face_mouse.
  Entity2D p;
  p.id = 1;
  p.name = "Player";
  p.player = PlayerControllerData{};
  const std::string plain = json::write(scene_json::entity_to_json(p));
  if (plain.find("face_mouse") != std::string::npos) return fail("2D JSON: face_mouse off stays out");
  p.player->face_mouse = true;
  Entity2D p2;
  if (!scene_json::entity_from_json(scene_json::entity_to_json(p), &p2, &err) || !p2.player ||
      !p2.player->face_mouse) {
    return fail("2D JSON: face_mouse round trip " + err);
  }
  // The Empty templates ship with it on.
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path base = fs::temp_directory_path() / ("ts_mouselook_" + std::to_string(stamp));
  std::error_code ec;
  for (const char* id : {"empty-3d-fps", "empty-3d-tps"}) {
    const fs::path d = base / id;
    if (!editor::empty_templates::write(id, d.string(), id, &err)) return fail(std::string(id) + ": " + err);
    s3::Scene3D s;
    if (!s3::load_file(s3::scene_path(d.string()), &s, &err)) return fail(std::string(id) + ": " + err);
    const s3::Entity3D* pl = s.find_named("Player");
    if (!pl || !pl->player || pl->player->look != s3::Player3D::Look::Mouse || !pl->player->capture_cursor) {
      return fail(std::string(id) + ": the Player looks with the mouse");
    }
  }
  {
    const fs::path d = base / "empty-2d-topdown";
    if (!editor::empty_templates::write("empty-2d-topdown", d.string(), "td", &err)) return fail("topdown: " + err);
    scene_json::SceneDoc doc;
    if (!scene_json::load_file(scene_json::scene_path_for_project(d.string()), &doc, &err)) {
      return fail("topdown: " + err);
    }
    bool ok = false;
    for (const Entity2D& x : doc.entities) ok = ok || (x.player && x.player->face_mouse);
    if (!ok) return fail("empty-2d-topdown: the Player faces the mouse");
  }
  fs::remove_all(base, ec);
  *note += " JSON + templates ok;";
  return 0;
}

}  // namespace

int run_mouselook_smoke() {
  std::string note;
  if (first_person(&note) || third_person(&note) || top_down(&note) || round_trips(&note)) return 1;
  std::cout << "[smoke] mouse look OK (" << note.substr(note.empty() ? 0 : 1) << ")\n";
  return 0;
}

}  // namespace tombstone
}  // namespace ts
