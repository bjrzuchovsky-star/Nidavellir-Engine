#include "Smoke3D.h"

#include "render3d/Gltf.h"
#include "render3d/GlRenderer3D.h"
#include "render3d/HiddenGl.h"
#include "render3d/Scene3D.h"
#include "render3d/SceneView.h"
#include "scene/SceneJson.h"
#include "world3d/World3D.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;
namespace w3 = ts::tombstone::world3d;

int run_3d_editor_smoke();  // Smoke3DEditor.cpp

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] 3D FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// Every triangle faces away from the centre (closed meshes) / up (plane).
bool outward(const r3::MeshData& m, bool plane) {
  for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    const r3::Vec3 a = m.vertices[m.indices[i]].position, b = m.vertices[m.indices[i + 1]].position,
                   c = m.vertices[m.indices[i + 2]].position;
    const r3::Vec3 n = r3::cross(b - a, c - a);
    if (r3::length(n) < 1e-6f) continue;  // pole slivers
    const r3::Vec3 ref = plane ? r3::Vec3{0, 1, 0} : (a + b + c) * (1.0f / 3.0f);
    if (r3::dot(n, ref) <= 0.0f) return false;
  }
  return true;
}

int math_and_primitives() {
  const r3::Vec3 e{20.0f, 135.0f, -10.0f};
  const r3::Vec3 back = r3::Quat::from_euler_deg(e).to_euler_deg();
  if (!near(back.x, e.x, 0.01f) || !near(back.y, e.y, 0.01f) || !near(back.z, e.z, 0.01f)) {
    return fail("euler round trip");
  }
  const r3::Vec3 v = r3::Quat::axis_angle({0, 1, 0}, r3::radians(90)).rotate({1, 0, 0});
  if (!near(v.z, -1.0f, 1e-4f)) return fail("quat rotate");
  const r3::Mat4 m = r3::Mat4::trs({1, 2, 3}, r3::Quat::from_euler_deg({10, 20, 30}), {2, 3, 4});
  const r3::Vec3 p = m.inverse().transform_point(m.transform_point({0.3f, -0.7f, 1.1f}));
  if (!near(p.x, 0.3f, 1e-4f) || !near(p.y, -0.7f, 1e-4f) || !near(p.z, 1.1f, 1e-4f)) {
    return fail("matrix inverse");
  }
  for (const std::string& name : r3::primitive_names()) {
    r3::MeshData mesh;
    if (!r3::make_primitive("primitive:" + name, &mesh) || !mesh.valid()) {
      return fail("primitive " + name);
    }
    if (!outward(mesh, name == "plane")) return fail("primitive " + name + " winding");
  }
  r3::Camera3D cam;
  float sx = 0, sy = 0;
  if (!cam.project(cam.target, 640, 360, &sx, &sy) || !near(sx, 320, 0.5f) || !near(sy, 180, 0.5f)) {
    return fail("camera project");
  }
  const r3::Ray ray = cam.screen_ray(320, 180, 640, 360);
  if (r3::dot(ray.dir, cam.forward()) < 0.999f) return fail("camera screen ray");
  std::cout << "[smoke] 3D math OK (euler/quat/inverse, cube/plane/sphere/capsule winding, "
               "camera rays)\n";
  return 0;
}

int scene_roundtrip(const fs::path& root) {
  s3::Scene3D s = s3::make_starter_scene();
  s.entities[1].transform.rotation = r3::Quat::from_euler_deg({0, 33.3f, 0});
  s.entities[1].script = ts::tombstone::ScriptData{};
  s.entities[1].script->path = "scripts/crate.lua";
  s.entities[1].script->set("speed", ts::tombstone::ScriptValue::of_number(2.5));
  s.entities[1].script->set("label", ts::tombstone::ScriptValue::of_text("Crate \"A\""));
  s.entities[1].material->albedo_texture = "assets/boards.tga";
  s.selection = {2};
  // v8: VR components ride along.
  s.entities[1].grabbable = s3::Grabbable3D{};
  s.entities[1].grabbable->throw_scale = 1.5f;
  s.entities[3].vr_rig = s3::VrRig3D{};
  s.entities[3].vr_rig->locomotion = s3::VrRig3D::Locomotion::Teleport;
  s.entities[3].vr_rig->snap_turn_deg = 30.0f;
  const std::string text = s3::write(s);
  s3::Scene3D back;
  std::string err;
  if (!s3::parse(text, &back, &err)) return fail("parse own output: " + err);
  if (!(back == s)) return fail("scene.json v8 round trip changed the scene");
  if (s3::write(back) != text) return fail("scene.json v8 not byte-stable");
  if (text.find("\"version\": 8") == std::string::npos || !s3::is_3d_scene_text(text)) {
    return fail("v8 header");
  }
  // 2D and 3D refuse each other's files with a clear reason.
  ts::tombstone::scene_json::SceneDoc doc2d;
  if (ts::tombstone::scene_json::parse(text, &doc2d, &err)) {
    return fail("2D reader accepted a 3D scene");
  }
  if (err.find("3D") == std::string::npos) return fail("2D reader reason: " + err);
  const std::string text2d = ts::tombstone::scene_json::write(ts::tombstone::scene_json::SceneDoc{});
  if (s3::parse(text2d, &back, &err) || err.find("2D") == std::string::npos) {
    return fail("3D reader accepted a 2D scene");
  }
  const fs::path file = root / "_scene3d_roundtrip" / "scene.json";
  if (!s3::save_file(file.string(), s, &err) || !s3::load_file(file.string(), &back, &err) ||
      !(back == s)) {
    return fail("scene.json v8 file round trip: " + err);
  }
  std::cout << "[smoke] 3D scene.json v8 round trip OK (" << s.entities.size()
            << " entities, byte-stable; 2D/3D readers refuse each other)\n";
  return 0;
}

int gltf_and_sample(const fs::path& dir) {
  std::string err;
  if (!s3::write_ghost_town_sample(dir.string(), "Ghost Town 3D", &err)) {
    return fail("ghost-town sample: " + err);
  }
  r3::Model model;
  if (!r3::load_gltf((dir / "assets" / "lantern.gltf").string(), &model, &err)) {
    return fail("glTF load: " + err);
  }
  if (model.primitives.size() != 3 || model.materials.size() != 3) {
    return fail("glTF lantern: expected 3 primitives / 3 materials");
  }
  if (!(model.materials[2].emissive[0] > 0.9f) || !(model.materials[0].metallic > 0.8f)) {
    return fail("glTF lantern materials");
  }
  if (!near(model.bounds.max.y, 2.4f, 0.01f) || model.triangles() != 36) {
    return fail("glTF lantern bounds / triangles");
  }
  if (r3::load_gltf((dir / "assets" / "nope.gltf").string(), &model, &err) ||
      err.find("not found") == std::string::npos) {
    return fail("missing glTF should say file not found: " + err);
  }
  r3::Image img;
  if (!r3::load_image_file((dir / "assets" / "boards.tga").string(), &img, &err) ||
      img.width != 64 || img.height != 64) {
    return fail("boards.tga: " + err);
  }
  s3::Scene3D scene;
  if (!s3::load_file(s3::scene_path(dir.string()), &scene, &err)) return fail("sample scene: " + err);
  r3::SceneView view(nullptr);
  view.set_project_dir(dir.string());
  const r3::Aabb lb = view.mesh_bounds("assets/lantern.gltf");
  if (!near(lb.max.y, 2.4f, 0.01f)) return fail("SceneView glTF bounds");
  std::cout << "[smoke] 3D glTF OK (lantern.gltf: 3 primitives, 36 tris, metal + emissive "
               "materials; boards.tga 64x64; ghost-town-3d sample "
            << scene.entities.size() << " entities)\n";
  return 0;
}

int world_runtime(const fs::path& dir) {
  s3::Scene3D scene;
  std::string err;
  if (!s3::load_file(s3::scene_path(dir.string()), &scene, &err)) return fail(err);
  std::vector<std::string> lines;
  w3::World3D w;
  w.set_log_sink([&](const ts::tombstone::runtime::LogEntry& e) {
    lines.push_back(ts::tombstone::runtime::format_log(e, true));
  });
  if (!w.start(scene, dir.string(), &err)) return fail("World3D start: " + err);
  const std::uint64_t rider = w.player_id();
  if (!rider || w.static_colliders() < 8 || w.script_count() != 2) {
    return fail("World3D setup (rider / statics / scripts)");
  }
  w3::Input3D idle;
  w.run_ticks(30, idle);
  const r3::Vec3 settled = w.find(rider)->transform.position;
  if (!w.grounded() || !near(settled.y, 1.0f, 0.05f)) {
    return fail("rider should stand on the ground at y=1, got y=" + std::to_string(settled.y));
  }
  // Sails spin through Lua (spin about z).
  const std::uint64_t sails = w.script_find("Windmill Sails");
  const r3::Quat q = w.find(sails)->transform.rotation;
  if (std::fabs(q.z) < 0.1f) return fail("windmill script did not turn the sails");
  // Jump.
  w3::Input3D jump;
  jump.jump = true;
  w.step(jump);
  w.run_ticks(10, idle);
  const float peak = w.find(rider)->transform.position.y;
  w.run_ticks(80, idle);
  if (!(peak > 1.5f) || !w.grounded() || !near(w.find(rider)->transform.position.y, 1.0f, 0.05f)) {
    return fail("jump and land");
  }
  // Ride north (camera yaw 0 -> forward is -z) under the bell (trigger).
  w.stop();
  scene.editor_camera.yaw = 0.0f;
  if (!w.start(scene, dir.string(), &err)) return fail(err);
  w3::Input3D north;
  north.move_forward = 1.0f;
  w.run_ticks(60 * 4, north);
  const r3::Vec3 after = w.find(rider)->transform.position;
  if (!(after.z < -10.5f)) return fail("rider should ride north past the bell, z=" + std::to_string(after.z));
  const std::uint64_t bell = w.script_find("Bell");
  const ts::tombstone::ScriptValue* rings = w.state(bell, "rings");
  if (!rings || rings->number != 1.0 || w.trigger_events() < 2) {
    return fail("bell trigger (enter + exit, script counted rings)");
  }
  bool toast = false;
  for (const w3::Toast3D& t : w.take_toasts()) toast = toast || t.text.find("rang the bell") != std::string::npos;
  if (!toast) return fail("bell toast");
  // West into the Saloon wall (x from -8.5 to -3.5, z -7..-1): blocked.
  w.stop();
  for (s3::Entity3D& e : scene.entities) {
    if (e.player) e.transform.position = {0.0f, 1.0f, -4.0f};
  }
  if (!w.start(scene, dir.string(), &err)) return fail(err);
  w3::Input3D west;
  west.move_right = -1.0f;
  w.run_ticks(120, west);
  const r3::Vec3 wall = w.find(rider)->transform.position;
  if (!(wall.x > -3.15f && wall.x < -3.0f) || !near(wall.z, -4.0f, 0.05f) || !w.grounded()) {
    return fail("Saloon wall should stop the rider at x=-3.1, got x=" + std::to_string(wall.x));
  }
  // Follow camera sits behind and above the rider, looking at it.
  const r3::Camera3D& cam = w.camera();
  if (!(cam.position.y > wall.y + 1.0f) || r3::length(cam.target - (wall + r3::Vec3{0, 1, 0})) > 0.5f) {
    return fail("follow camera");
  }
  // Pure capsule-vs-box helper.
  const r3::Vec3 push = w3::capsule_box_push({0, 0.4f, 0}, {0, 1.6f, 0}, 0.4f, {{0.2f, 0, -1}, {2, 2, 1}});
  if (!near(push.x, -0.2f, 1e-4f)) return fail("capsule_box_push");
  std::cout << "[smoke] 3D play OK (rider grounded at y=1, jump peak " << peak
            << " m, rode past the bell: trigger enter/exit + Lua rings=1 + toast, "
            << "Saloon wall stopped at x=" << wall.x << ", windmill spun via self:spin, follow cam)\n";
  return 0;
}

struct Px {
  int r, g, b;
  int lum() const { return (r * 3 + g * 6 + b) / 10; }
};

Px pixel(const std::vector<unsigned char>& rgba, int w, int h, float sx, float sy) {
  const int x = std::clamp(static_cast<int>(sx), 0, w - 1);
  const int y = std::clamp(h - 1 - static_cast<int>(sy), 0, h - 1);  // bottom row first
  const unsigned char* p = &rgba[(static_cast<std::size_t>(y) * w + x) * 4];
  return {p[0], p[1], p[2]};
}

int gl_render(const fs::path& dir) {
  r3::HiddenGl ctx;
  std::string why;
  if (!ctx.open(&why)) {
    if (std::getenv("TS_SMOKE_REQUIRE_GL")) return fail("GL context: " + why);
    std::cout << "[smoke] 3D render SKIPPED (" << why << "; set TS_SMOKE_REQUIRE_GL=1 to fail)\n";
    return 0;
  }
  auto renderer = r3::make_gl_renderer3d();
  if (!renderer->init(&why)) return fail("GL renderer init: " + why);
  renderer->set_offscreen(true);
  const int W = 320, H = 180;

  // Test scene: red cube on a pale ground, sun from up-left-front.
  s3::Scene3D s;
  s3::Entity3D ground;
  ground.id = 1;
  ground.name = "Ground";
  ground.transform.scale = {40, 1, 40};
  ground.mesh = s3::MeshRef{"primitive:plane", true};
  ground.material = s3::Material3D{};
  for (int i = 0; i < 3; ++i) ground.material->albedo[i] = 0.8f;
  ground.material->roughness = 0.9f;
  s3::Entity3D cube;
  cube.id = 2;
  cube.name = "Cube";
  cube.transform.position = {0, 0.5f, 0};
  cube.mesh = s3::MeshRef{"primitive:cube", true};
  cube.material = s3::Material3D{};
  cube.material->albedo[0] = 0.9f;
  cube.material->albedo[1] = 0.1f;
  cube.material->albedo[2] = 0.08f;
  s.entities = {ground, cube};
  // Sun behind-left, so the cube's shadow falls toward the camera, right.
  s.environment.sun_direction = {0.45f, -0.8f, 0.35f};
  r3::Camera3D cam;
  cam.position = {0, 3, 7};
  cam.target = {0, 0.5f, 0};
  r3::SceneView view(renderer.get());
  view.set_project_dir(dir.string());
  view.begin(s.environment, s.entities, cam, W, H);
  r3::FrameStats st = view.end();
  std::vector<unsigned char> px;
  int w = 0, h = 0;
  if (!renderer->read_pixels(&px, &w, &h) || w != W || h != H) return fail("read_pixels");
  float sx = 0, sy = 0;
  cam.project({0, 0.5f, 0.5f}, W, H, &sx, &sy);  // front face of the cube
  const Px red = pixel(px, w, h, sx, sy);
  const Px sky = pixel(px, w, h, W * 0.5f, 2);
  cam.project({0.75f, 0.0f, 0.85f}, W, H, &sx, &sy);  // in the cube's shadow
  const Px shade = pixel(px, w, h, sx, sy);
  cam.project({-1.8f, 0.0f, 0.85f}, W, H, &sx, &sy);  // open ground
  const Px lit = pixel(px, w, h, sx, sy);
  char buf[400];
  std::snprintf(buf, sizeof(buf),
                "cube rgb(%d,%d,%d) sky rgb(%d,%d,%d) shadow lum %d lit lum %d, %zu draws %zu tris",
                red.r, red.g, red.b, sky.r, sky.g, sky.b, shade.lum(), lit.lum(), st.draws,
                st.triangles);
  if (!(red.r > red.g + 60 && red.r > red.b + 60)) return fail(std::string("cube not red: ") + buf);
  // Top row looks above the horizon: between the sky's top and horizon
  // colors (gradient), nothing like the cube.
  const float* top = s.environment.sky_top;
  const float* hor = s.environment.sky_horizon;
  auto between = [](int v, float a, float b) {
    const int lo = static_cast<int>(std::min(a, b) * 255.0f) - 12;
    const int hi = static_cast<int>(std::max(a, b) * 255.0f) + 12;
    return v >= lo && v <= hi;
  };
  if (!between(sky.r, top[0], hor[0]) || !between(sky.g, top[1], hor[1]) ||
      !between(sky.b, top[2], hor[2])) {
    return fail(std::string("sky gradient: ") + buf);
  }
  if (!(shade.lum() * 4 < lit.lum() * 3)) return fail(std::string("shadow not darker: ") + buf);
  if (st.draws != 2 || st.shadow_casters != 2) return fail(std::string("stats: ") + buf);
  // A red point light near the ground brightens it red.
  s3::Entity3D lamp;
  lamp.id = 3;
  lamp.name = "Lamp";
  lamp.transform.position = {-2.5f, 0.6f, 2.0f};
  lamp.light = s3::Light3D{};
  lamp.light->color[0] = 1.0f;
  lamp.light->color[1] = 0.1f;
  lamp.light->color[2] = 0.05f;
  lamp.light->intensity = 12.0f;
  s.entities.push_back(lamp);
  cam.project({-2.5f, 0.0f, 2.0f}, W, H, &sx, &sy);
  const Px before = pixel(px, w, h, sx, sy);
  view.begin(s.environment, s.entities, cam, W, H);
  st = view.end();
  renderer->read_pixels(&px, &w, &h);
  const Px after = pixel(px, w, h, sx, sy);
  if (!(after.r > before.r + 25) || st.lights != 2) {
    return fail("point light did not light the ground red: before r=" + std::to_string(before.r) +
                " after r=" + std::to_string(after.r));
  }
  // The ghost town (glTF lanterns, textures, scripts' scene) renders clean.
  s3::Scene3D town;
  std::string err;
  if (!s3::load_file(s3::scene_path(dir.string()), &town, &err)) return fail(err);
  r3::Camera3D tcam;
  tcam.position = {14, 9, 18};
  tcam.target = {0, 1, 0};
  view.begin(town.environment, town.entities, tcam, W, H);
  const r3::FrameStats ts = view.end();
  const auto problems = view.take_problems();
  if (!problems.empty()) return fail("ghost town render: " + problems.front());
  if (ts.draws < 20 || ts.lights != 4) {
    return fail("ghost town stats: " + std::to_string(ts.draws) + " draws, " +
                std::to_string(ts.lights) + " lights");
  }
  view.clear();
  renderer->shutdown();
  std::cout << "[smoke] 3D render OK (GL " << ctx.version() << "; offscreen " << W << "x" << H
            << ": " << buf << "; point light r " << before.r << "->" << after.r
            << "; ghost town " << ts.draws << " draws, " << ts.triangles << " tris, "
            << ts.lights << " lights)\n";
  return 0;
}

}  // namespace

int run_3d_smoke() {
  const fs::path root = "./TombStoneProjects";
  const fs::path dir = root / "_ghost_town_smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);
  if (math_and_primitives() != 0) return 1;
  if (scene_roundtrip(root) != 0) return 1;
  if (gltf_and_sample(dir) != 0) return 1;
  if (world_runtime(dir) != 0) return 1;
  if (gl_render(dir) != 0) return 1;
  if (run_3d_editor_smoke() != 0) return 1;
  return 0;
}
