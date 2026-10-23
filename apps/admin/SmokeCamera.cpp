// --smoke-camera (see SmokeCamera.h). Drives the public calls the
// inspector, menus and keys use, without a window.
#include "SmokeCamera.h"

#include "SmokePluginSplit.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "editor/workspace/SceneIO.h"
#include "plugins/split/Split.h"
#include "render3d/Attach3D.h"
#include "render3d/CameraMigrate3D.h"
#include "render3d/Scene3D.h"
#include "runtime/GraphScript.h"
#include "runtime/World.h"
#include "scene/CameraMigrate.h"
#include "scene/SceneJson.h"
#include "world3d/World3D.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;
namespace w3 = ts::tombstone::world3d;
namespace rt = ts::tombstone::runtime;
using ts::tombstone::Entity2D;
using ts::tombstone::ScriptValue;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] camera FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool near3(const r3::Vec3& a, const r3::Vec3& b, float eps = 1e-3f) {
  return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
std::string f3(const r3::Vec3& v) {
  char b[96];
  std::snprintf(b, sizeof(b), "(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
  return b;
}

ed::ProjectInfo fresh_project(const std::string& id, ed::ProjectKind kind) {
  const fs::path dir = fs::absolute(fs::path("TombStoneProjects") / id);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  ed::ProjectInfo info;
  info.id = id;
  info.name = id;
  info.kind = kind;
  info.path = dir.string();
  return info;
}

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
void spit(const fs::path& p, const std::string& s) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}
// Rewrite the "version" of a saved scene (old-claim fixtures).
void set_version(const fs::path& p, int v) {
  const std::string s = slurp(p);
  spit(p, std::regex_replace(s, std::regex("\"version\"\\s*:\\s*[0-9]+"), "\"version\": " + std::to_string(v),
                             std::regex_constants::format_first_only));
}

std::uint64_t cam_named(const s3::Scene3D& s, const std::string& n) {
  const s3::Entity3D* e = s.find_named(n);
  return e && e->camera ? e->id : 0;
}

double num(const w3::World3D& w, std::uint64_t id, const char* key) {
  const auto* v = w.state(id, key);
  return v ? v->number : 0.0;
}

// ---- 3D editor ---------------------------------------------------------------

int editor3d() {
  const ed::ProjectInfo info = fresh_project("_camera3d_smoke", ed::ProjectKind::ThreeD);
  ed::Editor3DScreen e(info);
  e.on_enter();
  const std::uint64_t main_cam = cam_named(e.scene(), "Main Camera");
  if (!main_cam) return fail("3D starter has a Main Camera entity");
  const s3::Entity3D* mc = e.find(main_cam);
  if (mc->camera->rig != s3::PlayCamera3D::Rig::Orbit || mc->camera->follow == 0 ||
      !e.find(mc->camera->follow) || !e.find(mc->camera->follow)->player) {
    return fail("Main Camera orbits the rider");
  }
  if (e.play_camera_id() != main_cam) return fail("Main Camera is the Play camera");

  // Create: one undo step, selected, at the editor view.
  std::size_t depth = e.undo_depth();
  const std::uint64_t cam = e.create_camera();
  if (!cam || !e.find(cam) || !e.find(cam)->camera) return fail("create_camera");
  if (e.undo_depth() != depth + 1) return fail("create_camera = one undo step");
  if (e.selection().size() != 1 || e.selection()[0] != cam) return fail("new camera selected");
  if (!near3(e.find(cam)->transform.position, e.editor_camera().position, 1e-2f)) {
    return fail("new camera at the editor view");
  }
  // Inspector fields round-trip through the file.
  s3::PlayCamera3D pc = *e.find(cam)->camera;
  pc.projection = s3::PlayCamera3D::Projection::Orthographic;
  pc.fov = 42.0f;
  pc.near_clip = 0.5f;
  pc.far_clip = 250.0f;
  pc.ortho_size = 7.5f;
  pc.use_clear = true;
  pc.clear_color[0] = 0.2f;
  pc.clear_color[1] = 0.1f;
  pc.clear_color[2] = 0.05f;
  pc.viewport[0] = 0.5f;
  pc.viewport[2] = 0.5f;
  pc.viewport[3] = 0.5f;
  pc.priority = 3;
  depth = e.undo_depth();
  if (!e.set_camera(cam, pc, "Edit Camera") || e.undo_depth() != depth + 1) return fail("set_camera step");
  std::string err;
  if (!e.save_scene(&err)) return fail("save: " + err);
  s3::Scene3D back;
  if (!s3::load_file(s3::scene_path(info.path), &back, &err)) return fail("reload: " + err);
  if (!back.find(cam) || !back.find(cam)->camera || !(*back.find(cam)->camera == pc)) {
    return fail("camera fields round-trip scene.json");
  }
  if (back.legacy_cameras) return fail("v18 scene is not legacy");
  // camera_view follows the component.
  const r3::Camera3D cv = e.camera_view(*e.find(cam));
  if (!cv.orthographic || !near(cv.ortho_size, 7.5f) || !near(cv.z_near, 0.5f) || !near(cv.z_far, 250.0f)) {
    return fail("camera_view uses the component");
  }
  // Priority: 3 beats Main Camera's 0.
  if (e.play_camera_id() != cam) return fail("highest priority plays");
  // Inactive: drops out.
  pc.active = false;
  e.set_camera(cam, pc, "Edit Camera");
  if (e.play_camera_id() != main_cam) return fail("inactive camera skipped");
  depth = e.undo_depth();
  if (!e.make_active_camera(cam) || e.undo_depth() != depth + 1) return fail("make_active_camera step");
  if (e.play_camera_id() != cam || !e.find(cam)->camera->active) return fail("make_active_camera wins");
  e.undo();
  if (e.play_camera_id() != main_cam) return fail("undo make active");
  e.redo();
  if (e.play_camera_id() != cam) return fail("redo make active");

  // Gizmo move + parenting: a camera is a normal entity.
  r3::Transform t = e.find(cam)->transform;
  t.position = {2.0f, 3.0f, 9.0f};
  e.set_transform(cam, t, "Move");
  if (!near3(e.find(cam)->transform.position, t.position)) return fail("camera moves");
  const std::uint64_t post = e.create_empty({0.0f, 0.0f, 9.0f});
  if (!e.set_parent(cam, post) || e.find(cam)->parent != post) return fail("camera parents under an empty");
  r3::Transform pt = e.find(post)->transform;
  pt.position.x += 4.0f;
  e.set_transform(post, pt, "Move");
  if (!near3(e.find(cam)->transform.position, {6.0f, 3.0f, 9.0f})) return fail("parent carries the camera");
  e.set_parent(cam, 0);

  // Align camera to view: one step, the editor view's spot and heading.
  depth = e.undo_depth();
  e.select(cam);
  if (!e.align_camera_to_view(cam) || e.undo_depth() != depth + 1) return fail("align_camera_to_view step");
  const r3::Camera3D ev = e.editor_camera();
  const r3::Camera3D av = e.camera_view(*e.find(cam));
  const r3::Vec3 ef = r3::normalize(ev.target - ev.position), af = r3::normalize(av.target - av.position);
  if (!near3(av.position, ev.position, 1e-2f) || !near3(af, ef, 1e-2f)) {
    return fail("aligned to view " + f3(av.position) + " / " + f3(ev.position));
  }
  e.undo();
  if (near3(e.find(cam)->transform.position, ev.position, 1e-2f)) return fail("undo align");
  e.redo();
  // Look through: the editor view becomes the camera's; orbiting ends it.
  if (!e.look_through_camera(cam) || !e.looking_through()) return fail("look through");
  if (!near3(e.editor_camera().position, e.find(cam)->transform.position, 1e-3f) ||
      !e.editor_camera().orthographic) {
    return fail("editor view = the camera while looking through");
  }
  e.stop_looking_through();
  if (e.looking_through()) return fail("stop looking through");
  // Preview (render to texture). Headless boxes have no GL: the null
  // renderer answers, the call must not fall over either way.
  const bool preview = e.render_camera_preview(160, 90);
  std::cout << "[smoke] camera 3D preview: " << (preview ? "rendered" : "no GL (null renderer)") << " texture "
            << e.camera_preview_texture() << '\n';
  // Play: the active camera is the view; the camera entity has no mesh
  // (the editor icon / frustum are editor-only overlay lines).
  if (e.find(cam)->mesh) return fail("camera entity draws nothing in Play");
  if (!e.play_start()) return fail("play_start");
  e.play_ticks(3, w3::Input3D{});
  w3::World3D& w = e.world();
  if (w.active_camera() != cam) return fail("Play looks through the highest-priority camera");
  if (!w.camera().orthographic || !near(w.camera().ortho_size, 7.5f) || !w.camera_view().use_clear ||
      !near(w.camera_view().viewport[2], 0.5f) || !near(w.view_environment().sky_top[0], 0.2f, 1e-3f)) {
    return fail("Play uses projection / clear colour / viewport");
  }
  if (!near3(w.camera().position, e.find(cam)->transform.position, 1e-3f)) return fail("Fixed rig = entity");
  e.play_stop();
  if (e.play_camera_id() != cam) return fail("Play leaves the scene alone");
  std::cout << "[smoke] camera 3D editor ok (create / fields / priority / move / parent / align / look "
               "through / Play view / undo)\n";
  return 0;
}

// ---- 3D runtime: follow, scripts, collision, VR -------------------------------

int runtime3d() {
  const ed::ProjectInfo info = fresh_project("_camera3d_rt_smoke", ed::ProjectKind::ThreeD);
  s3::Scene3D s;
  {
    ed::Editor3DScreen e(info);
    e.on_enter();
    const std::uint64_t made = e.create_camera();  // "Camera", Fixed
    if (e.play_camera_id() == made) return fail("a new camera does not take over");
    e.make_active_camera(made);
    s = e.scene();
  }
  const std::uint64_t main_cam = cam_named(s, "Main Camera");
  const std::uint64_t fixed = cam_named(s, "Camera");
  if (!main_cam || !fixed) return fail("runtime cameras");
  std::string err;
  // Scripts: Lua switches to Main Camera after a few ticks.
  const s3::Entity3D* crate0 = s.find_named("Crate");
  if (!crate0) return fail("starter Crate");
  const std::uint64_t crate = crate0->id;
  for (s3::Entity3D& x : s.entities) {
    if (x.id == crate) {
      x.script = ts::tombstone::ScriptData{};
      x.script->path = "scripts/cams.lua";
    }
  }
  {
    w3::World3D w;
    w.scripts().put("scripts/cams.lua", R"lua(
local n = 0
function on_start()
  self:set("count", camera.count())
  self:set("first", camera.active())
end
function on_tick(dt)
  n = n + 1
  if n == 5 then self:set("ok", camera.set_active("Main Camera") and 1 or 0) end
  if n == 6 then self:set("now", camera.active()) end
  if n == 7 then self:set("bogus", camera.set_active("No Such Camera") and 1 or 0) end
end
)lua");
    if (!w.start(s, info.path, &err)) return fail("start: " + err);
    w.run_ticks(10, w3::Input3D{});
    if (num(w, crate, "count") != 2 || static_cast<std::uint64_t>(num(w, crate, "first")) != fixed) {
      return fail("Lua camera.count / active: count " + std::to_string(num(w, crate, "count")) + " first " +
                  std::to_string(num(w, crate, "first")) + " want " + std::to_string(fixed));
    }
    if (num(w, crate, "ok") != 1 || static_cast<std::uint64_t>(num(w, crate, "now")) != main_cam ||
        w.active_camera() != main_cam || num(w, crate, "bogus") != 0) {
      return fail("Lua camera.set_active");
    }
    // The bridge (graph nodes, C++ ts::camera, C# Camera go this way).
    ScriptValue r;
    if (!w.script_plugin_call("camera", "set_active", {ScriptValue::of_number(static_cast<double>(fixed))}, &r,
                              &err)) {
      return fail("bridge camera.set_active: " + err);
    }
    w.step(w3::Input3D{});
    if (w.active_camera() != fixed) return fail("bridge switched camera");
    w.script_plugin_call("camera", "reset", {}, &r, &err);
    w.step(w3::Input3D{});
    if (w.active_camera() != fixed) return fail("reset = highest priority again");
    if (!w.set_camera_priority(main_cam, 50)) return fail("set_camera_priority");
    w.step(w3::Input3D{});
    if (w.active_camera() != main_cam) return fail("priority change switches");
    // Orbit follows the rider: walk forward, the camera comes along at
    // about its boom length.
    const r3::Vec3 p0 = w.find(w.player_id())->transform.position;
    const r3::Vec3 c0 = w.camera().position;
    w3::Input3D fwd;
    fwd.move_forward = 1.0f;
    w.run_ticks(90, fwd);
    const r3::Vec3 p1 = w.find(w.player_id())->transform.position;
    const r3::Vec3 c1 = w.camera().position;
    if (r3::length(p1 - p0) < 1.0f) return fail("rider walked");
    if (r3::length(c1 - c0) < 0.5f * r3::length(p1 - p0)) return fail("orbit camera follows the rider");
    const float boom = r3::length(c1 - (p1 + r3::Vec3{0, 1, 0}));
    if (boom > s.find(main_cam)->camera->distance + 3.0f) return fail("boom length " + std::to_string(boom));
    // The entity rides with the view (children / scripts see it).
    if (!near3(w.find(main_cam)->transform.position, w.camera().position, 1e-3f)) {
      return fail("camera entity = the view");
    }
  }
  // Boom collision through the physics sphere cast (kinematic wall the
  // core AABBs don't know about), then the same without the plugin.
  auto boom_run = [&](bool add_wall, std::size_t* hits, float* dist) {
    s3::Scene3D sc = s;
    for (s3::Entity3D& x : sc.entities) {
      if (x.id == fixed) x.camera->active = false;
      if (x.id == crate) x.script.reset();
    }
    w3::World3D probe;
    if (!probe.start(sc, info.path, &err)) return false;
    probe.run_ticks(20, w3::Input3D{});
    const r3::Vec3 target = probe.find(probe.player_id())->transform.position + r3::Vec3{0, 1, 0};
    const r3::Vec3 cam_at = probe.camera().position;
    if (add_wall) {
      s3::Entity3D wall;
      wall.id = sc.next_id();
      wall.name = "Boom Wall";
      wall.transform.position = target + (cam_at - target) * 0.55f;
      wall.transform.scale = {3.0f, 3.0f, 3.0f};
      wall.collider = s3::Collider3D{};
      wall.rigid_body = s3::RigidBody3D{};
      wall.rigid_body->motion = s3::RigidBody3D::Motion::Kinematic;
      sc.entities.push_back(wall);
    }
    w3::World3D w;
    if (!w.start(sc, info.path, &err)) return false;
    w.run_ticks(20, w3::Input3D{});
    *hits = w.boom_hits();
    *dist = r3::length(w.camera().position - (w.find(w.player_id())->transform.position + r3::Vec3{0, 1, 0}));
    return true;
  };
  std::size_t hits_open = 0, hits_wall = 0, hits_off = 0;
  float d_open = 0, d_wall = 0, d_off = 0;
  if (!boom_run(false, &hits_open, &d_open) || !boom_run(true, &hits_wall, &d_wall)) return fail("boom: " + err);
  if (hits_wall == 0 || d_wall > d_open - 1.0f) {
    return fail("sphere cast pulls the boom in (hits " + std::to_string(hits_wall) + ", " + std::to_string(d_wall) +
                " vs " + std::to_string(d_open) + ")");
  }
  smoke_provider_hold().clear();
  const bool off_ok = boom_run(true, &hits_off, &d_off);
  smoke_provider_hold().hold(ts::tombstone::plugins::split_plugin_ids());
  if (!off_ok) return fail("boom without physics: " + err);
  if (hits_off != 0) return fail("no physics plugin = no sphere cast");
  // VR: the HMD drives the view; camera entities stay put.
  {
    w3::World3D w;
    w.set_vr(true);
    if (!w.start(s, info.path, &err)) return fail("VR start: " + err);
    const r3::Vec3 before = w.find(main_cam)->transform.position;
    w.run_ticks(10, w3::Input3D{});
    if (w.active_camera() != 0 || !near3(w.find(main_cam)->transform.position, before)) {
      return fail("VR keeps the HMD camera");
    }
  }
  std::cout << "[smoke] camera 3D runtime ok (Lua + bridge switch, priority, orbit follow, boom sphere cast "
            << hits_wall << " hits " << d_open << " m -> " << d_wall << " m, no-op without the plugin, VR = HMD)\n";
  return 0;
}

// ---- 3D migration --------------------------------------------------------------

int migration3d() {
  const ed::ProjectInfo info = fresh_project("_camera3d_mig_smoke", ed::ProjectKind::ThreeD);
  s3::Scene3D legacy;
  {
    ed::Editor3DScreen e(info);
    e.on_enter();
    legacy = e.scene();
  }
  // A pre-0.2.0 claim: no camera entities, version 17.
  std::erase_if(legacy.entities, [](const s3::Entity3D& x) { return x.camera.has_value(); });
  const fs::path file = s3::scene_path(info.path);
  std::string err;
  if (!s3::save_file(file.string(), legacy, &err)) return fail("save legacy: " + err);
  set_version(file, 17);
  s3::Scene3D old;
  if (!s3::load_file(file.string(), &old, &err) || !old.legacy_cameras) return fail("v17 loads as legacy");
  auto ride = [&](const s3::Scene3D& sc, std::vector<r3::Vec3>* out) {
    w3::World3D w;
    if (!w.start(sc, info.path, &err)) return false;
    for (int i = 0; i < 150; ++i) {
      w3::Input3D in;
      in.move_forward = i < 100 ? 1.0f : 0.0f;
      in.turn = (i > 40 && i < 70) ? 2.0f : 0.0f;
      w.step(in);
      out->push_back(w.camera().position);
      out->push_back(w.camera().target);
    }
    return true;
  };
  std::vector<r3::Vec3> a, b;
  if (!ride(old, &a)) return fail("legacy ride: " + err);
  // Open in the editor: migrate, back up, log.
  std::string backup;
  {
    ed::Editor3DScreen e(info);
    e.on_enter();
    backup = e.camera_migration_backup();
    const std::uint64_t mc = cam_named(e.scene(), "Main Camera");
    if (!mc || e.scene().legacy_cameras) return fail("migration added Main Camera");
    if (e.undo_depth() != 0) return fail("migration is not an undo step");
  }
  if (backup.empty() || !fs::exists(fs::path(info.path) / backup)) return fail("migration backup " + backup);
  {
    s3::Scene3D bk;
    if (!s3::load_file((fs::path(info.path) / backup).string(), &bk, &err) || !bk.legacy_cameras) {
      return fail("backup is the old scene");
    }
  }
  const std::string log = slurp(fs::path(info.path) / ".tombstone" / "migrations.log");
  if (log.find("cameras (v18)") == std::string::npos || log.find("Main Camera") == std::string::npos) {
    return fail("migrations.log: " + log);
  }
  s3::Scene3D now;
  if (!s3::load_file(file.string(), &now, &err) || now.legacy_cameras || !cam_named(now, "Main Camera")) {
    return fail("migrated file saved as v18");
  }
  if (!ride(now, &b) || a.size() != b.size()) return fail("migrated ride: " + err);
  float worst = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, r3::length(a[i] - b[i]));
  if (worst > 1e-3f) return fail("migrated view differs from the old follow camera by " + std::to_string(worst) + " m");
  // Opening again: nothing more to do, no second backup.
  {
    ed::Editor3DScreen e(info);
    e.on_enter();
    if (!e.camera_migration_backup().empty()) return fail("second open migrates again");
  }
  std::cout << "[smoke] camera 3D migration ok (backup " << backup << ", log, view identical over " << a.size() / 2
            << " ticks, max diff " << worst << " m)\n";
  return 0;
}

// ---- 2D ----------------------------------------------------------------------------

int editor2d() {
  ed::Editor2DScreen scr(fresh_project("_camera2d_smoke", ed::ProjectKind::TwoD));
  scr.on_enter();
  ed::Workspace2D& ws = scr.workspace();
  std::uint64_t seeded = 0, rider = 0;
  for (const Entity2D& x : ws.entities()) {
    if (x.camera && !seeded) seeded = x.id;
    if (x.player && !rider) rider = x.id;
  }
  if (!seeded || !rider) return fail("2D seed has a camera + rider");
  if (scr.play_camera_2d() != seeded) return fail("2D seeded camera plays");
  std::size_t depth = ws.undo_count();
  const std::uint64_t cam = scr.create_camera_2d();
  if (!cam || ws.undo_count() != depth + 1 || !ws.find(cam)->camera) return fail("2D create camera step");
  if (ws.find(cam)->camera->target != rider) return fail("2D new camera follows the rider");
  if (scr.play_camera_2d() != seeded) return fail("2D new camera does not take over");
  scr.undo();
  if (ws.find(cam)) return fail("2D undo create camera");
  scr.redo();
  if (!ws.find(cam)) return fail("2D redo create camera");
  depth = ws.undo_count();
  if (!scr.make_active_camera_2d(cam) || ws.undo_count() != depth + 1 || scr.play_camera_2d() != cam) {
    return fail("2D make active camera");
  }
  scr.undo();
  if (scr.play_camera_2d() != seeded) return fail("2D undo make active");
  scr.redo();
  // Fields round-trip.
  Entity2D* c = ws.find(cam);
  c->camera->zoom = 2.0f;
  c->camera->smoothing = 0.0f;
  c->camera->dead_zone_w = 64.0f;
  c->camera->dead_zone_h = 32.0f;
  c->camera->use_clear = true;
  c->camera->clear_color[1] = 0.5f;
  c->camera->viewport[2] = 0.5f;
  c->camera->priority = 9;
  c->camera->use_bounds = true;
  c->camera->bounds_x = -2000.0f;
  c->camera->bounds_y = -2000.0f;
  c->camera->bounds_w = 4000.0f;
  c->camera->bounds_h = 4000.0f;
  const ts::tombstone::Camera2DData want = *c->camera;
  std::string err;
  if (!ed::scene_io::save(ws, scr.scene_path(), &err)) return fail("2D save: " + err);
  ts::tombstone::scene_json::SceneDoc doc;
  if (!ts::tombstone::scene_json::load_file(scr.scene_path(), &doc, &err)) return fail("2D reload: " + err);
  bool found = false;
  for (const Entity2D& x : doc.entities) found |= x.id == cam && x.camera && *x.camera == want;
  if (!found) return fail("2D camera fields round-trip");

  // Runtime: highest priority, follow + dead zone, zoom, scripts.
  std::vector<Entity2D> ents = ws.entities();
  for (Entity2D& x : ents) {
    x.script.reset();  // starter scripts are not on disk in this bare world
    if (x.id == rider) {
      x.script = ts::tombstone::ScriptData{};
      x.script->path = "scripts/cams.lua";
    }
    if (x.id == seeded) {
      x.script = ts::tombstone::ScriptData{};
      x.script->path = "scripts/cams.graph.json";
    }
  }
  rt::World w;
  w.script_library().put("scripts/cams.lua",
                         "local n = 0\n"
                         "function on_tick(dt)\n"
                         "  n = n + 1\n"
                         "  if n == 10 then log('count=' .. camera.count() .. ' ok=' .. tostring(camera.set_active('" +
                             ws.find(cam)->name + "'))) end\n"
                         "  if n == 11 then log('now=' .. camera.active()) end\n"
                         "end\n");
  rt::Graph g;
  auto node = [&g](const std::string& type, std::map<std::string, ScriptValue> in) {
    rt::GraphNode n;
    n.id = g.next_id();
    n.type = type;
    n.inputs = std::move(in);
    g.nodes.push_back(n);
    return n.id;
  };
  const int start = node("event.start", {});
  const int set = node("camera.set_active", {{"camera", ScriptValue::of_number(static_cast<double>(seeded))}});
  g.links.push_back({start, "out", set, "in"});
  w.script_library().put("scripts/cams.graph.json", rt::write_graph(g));
  if (!w.build(ents, "", &err)) return fail("2D world: " + err);
  w.set_view_size(1280.0f, 720.0f);
  w.step(rt::InputFrame{});
  w.step(rt::InputFrame{});
  if (w.camera_entity() != seeded) return fail("2D graph Set Active Camera (" + std::to_string(w.camera_entity()) + ")");
  for (int i = 0; i < 10; ++i) w.step(rt::InputFrame{});
  std::string logs;
  for (const auto& l : w.take_logs()) logs += l.text + "\n";
  if (logs.find("count=2 ok=true") == std::string::npos ||
      logs.find("now=" + std::to_string(cam)) == std::string::npos || w.camera_entity() != cam) {
    return fail("2D Lua camera.set_active: " + logs);
  }
  // Zoom 2: the view shows half the width.
  const auto rect = rt::World::view_rect(w.camera(), 1280.0f, 720.0f);
  if (!near(rect.x1 - rect.x0, 640.0f, 0.5f)) return fail("2D zoom view rect " + std::to_string(rect.x1 - rect.x0));
  if (!w.camera().use_clear || !near(w.camera().viewport[2], 0.5f)) return fail("2D clear / viewport");
  // Dead zone: a small step inside it leaves the camera still; a long ride
  // drags it along.
  rt::InputFrame right;
  right.slot(0) = rt::digital_input(false, true, false, false);
  const float cx0 = w.camera().x;
  w.step(right);
  w.step(right);
  if (!near(w.camera().x, cx0, 0.01f)) return fail("2D dead zone holds the camera");
  for (int i = 0; i < 90; ++i) w.step(right);
  if (w.camera().x < cx0 + 20.0f) return fail("2D camera follows past the dead zone");
  // Bridge.
  ScriptValue r;
  if (!w.script_plugin_call("camera", "set_active", {ScriptValue::of_number(static_cast<double>(seeded))}, &r, &err) ||
      (w.step(rt::InputFrame{}), w.camera_entity() != seeded)) {
    return fail("2D bridge camera.set_active: " + err);
  }
  std::cout << "[smoke] camera 2D ok (create / make active / undo / fields / priority / Lua + graph + bridge / "
               "zoom / dead zone / follow)\n";
  return 0;
}

int migration2d() {
  const ed::ProjectInfo info = fresh_project("_camera2d_mig_smoke", ed::ProjectKind::TwoD);
  std::vector<Entity2D> legacy;
  {
    ed::Editor2DScreen scr(info);
    scr.on_enter();
    for (const Entity2D& x : scr.workspace().entities())
      if (!x.camera) legacy.push_back(x);
  }
  const std::string file = ts::tombstone::scene_json::scene_path_for_project(info.path);
  ts::tombstone::scene_json::SceneDoc doc;
  doc.entities = legacy;
  std::string err;
  if (!ts::tombstone::scene_json::save_file(file, doc, &err)) return fail("2D legacy save: " + err);
  set_version(file, 12);
  auto ride = [&](const std::vector<Entity2D>& ents, std::vector<float>* out) {
    rt::World w;
    if (!w.build(ents, info.path, &err)) return false;
    w.set_view_size(1280.0f, 720.0f);
    for (int i = 0; i < 120; ++i) {
      rt::InputFrame in;
      in.slot(0) = rt::digital_input(i > 60, i < 60, i % 30 < 10, false);
      w.step(in);
      out->push_back(w.camera().x);
      out->push_back(w.camera().y);
      out->push_back(w.camera().zoom);
    }
    return true;
  };
  std::vector<float> a, b;
  if (!ride(legacy, &a)) return fail("2D legacy ride: " + err);
  std::string backup;
  std::vector<Entity2D> migrated;
  {
    ed::Editor2DScreen scr(info);
    scr.on_enter();
    backup = scr.camera_migration_backup_2d();
    migrated = scr.workspace().entities();
  }
  std::uint64_t cam = 0;
  for (const Entity2D& x : migrated)
    if (x.camera) cam = x.id;
  if (!cam) return fail("2D migration adds a Camera entity");
  if (backup.empty() || !fs::exists(fs::path(info.path) / backup)) return fail("2D backup " + backup);
  const std::string log = slurp(fs::path(info.path) / ".tombstone" / "migrations.log");
  if (log.find("cameras (v13)") == std::string::npos) return fail("2D migrations.log: " + log);
  ts::tombstone::scene_json::SceneDoc now;
  if (!ts::tombstone::scene_json::load_file(file, &now, &err) || now.version != 13) return fail("2D saved as v13");
  if (!ride(migrated, &b)) return fail("2D migrated ride: " + err);
  float worst = 0.0f;
  for (std::size_t i = 0; i < a.size(); ++i) worst = std::max(worst, std::fabs(a[i] - b[i]));
  if (worst > 1e-3f) return fail("2D migrated view differs by " + std::to_string(worst));
  {
    ed::Editor2DScreen scr(info);
    scr.on_enter();
    if (!scr.camera_migration_backup_2d().empty()) return fail("2D second open migrates again");
  }
  std::cout << "[smoke] camera 2D migration ok (backup " << backup << ", log, view identical over 120 ticks)\n";
  return 0;
}

}  // namespace

int run_camera_smoke() {
  if (editor3d() != 0) return 1;
  if (runtime3d() != 0) return 1;
  if (migration3d() != 0) return 1;
  if (editor2d() != 0) return 1;
  if (migration2d() != 0) return 1;
  std::cout << "[smoke] camera pass ok\n";
  return 0;
}
