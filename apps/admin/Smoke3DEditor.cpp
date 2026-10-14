// 3D editor smoke (no window, null renderer): starter scene, primitives,
// gizmo translate / rotate / scale with snap, undo / redo, picking, glTF
// drop, save / reload, Play + Stop restoring the scene.
#include "editor/editor3d/Editor3DScreen.h"
#include "render3d/Gltf.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] 3D editor FAILED: " << what << '\n';
  return 1;
}

bool on_step(float v, float step) {
  const float k = v / step;
  return std::fabs(k - std::round(k)) < 1e-3f;
}

constexpr float kW = 800.0f, kH = 600.0f;

// Screen point along a gizmo axis handle (fraction of its length).
bool handle_point(ed::Editor3DScreen& e, int axis, float frac, float* x, float* y) {
  const s3::Entity3D* s = e.find(e.primary());
  const r3::Camera3D cam = e.editor_camera();
  const float len = ed::Gizmo3D::handle_length(cam, kH, s->transform.position);
  return cam.project(s->transform.position + ed::Gizmo3D::axis_dir(axis) * (len * frac), kW, kH,
                     x, y);
}

}  // namespace

int run_3d_editor_smoke() {
  const fs::path dir = fs::absolute("TombStoneProjects/_editor3d_smoke");
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "assets", ec);
  std::string err;
  if (!r3::write_sample_lantern_gltf((dir / "assets/lantern.gltf").string(), &err)) {
    return fail("lantern: " + err);
  }
  ed::ProjectInfo info;
  info.id = "_editor3d_smoke";
  info.name = "Editor3D Smoke";
  info.kind = ed::ProjectKind::ThreeD;
  info.path = dir.string();

  ed::Editor3DScreen e(info);
  e.on_enter();
  if (!fs::exists(dir / "scene.json") || !e.scene().find_named("Crate")) {
    return fail("starter scene written on first open");
  }
  if (std::string(e.renderer_backend()) != "null") return fail("null renderer without a window");
  const std::size_t start_count = e.scene().entities.size();

  // Primitives.
  for (const char* p : {"cube", "plane", "sphere", "capsule"}) {
    if (!e.create_primitive(p, {3.0f, 0.0f, -2.0f})) return fail(std::string("create ") + p);
  }
  if (e.scene().entities.size() != start_count + 4 || e.undo_depth() != 4) {
    return fail("four primitives, four undo steps");
  }
  if (e.render_frame(320, 240).draws < start_count) return fail("render_frame draws");

  // Pick the crate by clicking its projected centre.
  const s3::Entity3D* crate = e.scene().find_named("Crate");
  const std::uint64_t crate_id = crate->id;
  float cx = 0, cy = 0;
  if (!e.editor_camera().project(crate->transform.position, kW, kH, &cx, &cy) ||
      e.pick(cx, cy, kW, kH) != crate_id) {
    return fail("click-pick the crate");
  }
  e.select(crate_id);
  const r3::Transform before = crate->transform;

  // Translate along X with snap.
  e.set_snapping(true);
  e.gizmo().mode = ed::GizmoMode::Translate;
  float hx, hy, fx, fy;
  if (!handle_point(e, 0, 0.7f, &hx, &hy) || !handle_point(e, 0, 1.0f, &fx, &fy)) {
    return fail("project the X handle");
  }
  if (!e.gizmo_begin(hx, hy, kW, kH) || e.gizmo().axis() != 0) return fail("grab the X handle");
  const float dx = fx - hx, dy = fy - hy;
  e.gizmo_move(hx + dx * 4.3f, hy + dy * 4.3f, kW, kH);
  e.gizmo_end();
  const r3::Transform moved = e.find(crate_id)->transform;
  if (!(moved.position.x > before.position.x + 0.4f) || !on_step(moved.position.x, 0.5f) ||
      moved.position.y != before.position.y || moved.position.z != before.position.z) {
    return fail("translate X snapped to 0.5 (x=" + std::to_string(moved.position.x) + ")");
  }

  // Rotate around Y: sweep the mouse around the origin's screen point.
  e.gizmo().mode = ed::GizmoMode::Rotate;
  float ox, oy;
  e.editor_camera().project(moved.position, kW, kH, &ox, &oy);
  if (!e.gizmo_begin_axis(1, ox + 60.0f, oy, kW, kH)) return fail("begin rotate");
  e.gizmo_move(ox + 30.0f, oy + 52.0f, kW, kH);  // ~60 degrees on screen
  e.gizmo_end();
  const r3::Vec3 eul = e.find(crate_id)->transform.rotation.to_euler_deg();
  if (std::fabs(eul.y) < 10.0f || !on_step(std::round(eul.y * 100.0f) / 100.0f, 15.0f)) {
    return fail("rotate Y snapped to 15 deg (yaw=" + std::to_string(eul.y) + ")");
  }

  // Scale along X.
  e.gizmo().mode = ed::GizmoMode::Scale;
  if (!handle_point(e, 0, 0.8f, &hx, &hy) || !handle_point(e, 0, 1.0f, &fx, &fy)) {
    return fail("project the scale handle");
  }
  if (!e.gizmo_begin(hx, hy, kW, kH)) return fail("grab the scale handle");
  e.gizmo_move(hx + (fx - hx) * 6.0f, hy + (fy - hy) * 6.0f, kW, kH);
  e.gizmo_end();
  const r3::Transform scaled = e.find(crate_id)->transform;
  if (!(scaled.scale.x > before.scale.x) || !on_step(scaled.scale.x, 0.25f)) {
    return fail("scale X snapped to 0.25 (sx=" + std::to_string(scaled.scale.x) + ")");
  }
  if (e.undo_depth() != 7) return fail("three gizmo drags, three undo steps");

  // Undo / redo.
  e.undo();
  e.undo();
  e.undo();
  if (!(e.find(crate_id)->transform == before)) return fail("undo x3 restores the crate");
  e.redo();
  if (!(e.find(crate_id)->transform == moved)) return fail("redo the move");

  // Drop a glTF from the asset list.
  bool listed = false;
  for (const auto& a : e.assets()) listed |= a.model && a.rel == "assets/lantern.gltf";
  if (!listed) return fail("assets panel lists .gltf");
  const std::uint64_t lantern = e.drop_asset("assets/lantern.gltf", {-2.0f, 0.0f, 1.0f});
  const s3::Entity3D* le = e.find(lantern);
  if (!le || !le->mesh || le->mesh->source != "assets/lantern.gltf" || !le->collider ||
      le->collider->size.y < 2.0f) {
    return fail("dropped glTF entity with a hugging collider");
  }

  // Save / reload round trip.
  const s3::Scene3D saved = e.scene();
  if (!e.save_scene(&err) || !e.reload_scene(&err)) return fail("save / reload: " + err);
  if (!(e.scene().entities == saved.entities)) return fail("reload changed the entities");

  // Play + Stop leaves the authored scene alone.
  if (!e.play_start() || !e.playing()) return fail("play");
  ts::tombstone::world3d::Input3D in;
  in.move_forward = 1.0f;
  e.play_ticks(60, in);
  if (e.render_frame(160, 120).draws == 0) return fail("play frame");
  e.play_stop();
  if (e.playing() || !(e.scene().entities == saved.entities)) return fail("stop restores the scene");

  e.on_exit();
  fs::remove_all(dir, ec);
  std::cout << "[smoke] 3D editor: starter scene, primitives, pick, gizmo move/rotate/scale "
               "with snap, undo/redo, glTF drop, save/reload, play/stop OK\n";
  return 0;
}
