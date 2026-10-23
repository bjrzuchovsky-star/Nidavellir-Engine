// --smoke-attach (see SmokeCamera.h). The attach tools in the 3D and 2D
// editors through the same calls the menus, Hierarchy and eyedropper use.
#include "SmokeCamera.h"

#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "render3d/Animator.h"
#include "render3d/Attach3D.h"
#include "render3d/Scene3D.h"
#include "runtime/World.h"
#include "world3d/World3D.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;
namespace w3 = ts::tombstone::world3d;
namespace rt = ts::tombstone::runtime;
using ts::tombstone::Entity2D;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] attach FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool near3(const r3::Vec3& a, const r3::Vec3& b, float eps = 1e-3f) {
  return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
bool sameq(const r3::Quat& a, const r3::Quat& b, float eps = 1e-3f) {
  const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
  return d > 1.0f - eps;
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

// ---- 3D ---------------------------------------------------------------------

int attach3d() {
  ed::Editor3DScreen e(fresh_project("_attach3d_smoke", ed::ProjectKind::ThreeD));
  e.on_enter();
  e.set_snapping(false);
  const std::uint64_t a = e.create_empty({1.0f, 1.0f, 1.0f});
  const std::uint64_t b = e.create_empty({5.0f, 2.0f, -3.0f});
  const std::uint64_t c = e.create_empty({-2.0f, 0.5f, 4.0f});
  r3::Transform tb = e.find(b)->transform;
  tb.rotation = r3::Quat::axis_angle({0, 1, 0}, r3::radians(90.0f));
  e.set_transform(b, tb, "Turn");
  const r3::Transform ta0 = e.find(a)->transform;
  auto T = [&](std::uint64_t id) { return e.find(id)->transform; };

  // Attach keep: parent only, world transform unchanged, one step.
  e.select(a);
  std::size_t depth = e.undo_depth();
  if (!e.attach_selected(b, s3::AttachMode::KeepWorld) || e.undo_depth() != depth + 1) return fail("attach keep step");
  if (e.find(a)->parent != b || !near3(T(a).position, ta0.position)) return fail("attach keep");
  // Children ride the parent.
  tb.position.y += 2.0f;
  e.set_transform(b, tb, "Lift");
  if (!near3(T(a).position, ta0.position + r3::Vec3{0, 2, 0})) return fail("attached child rides along");
  // Detach keeps the world transform.
  const r3::Transform ta1 = T(a);
  depth = e.undo_depth();
  if (!e.detach_selected() || e.undo_depth() != depth + 1 || e.find(a)->parent != 0 ||
      !near3(T(a).position, ta1.position)) {
    return fail("detach keeps the world transform");
  }
  e.undo();
  if (e.find(a)->parent != b) return fail("undo detach");
  e.undo();  // the lift
  e.undo();  // the attach
  if (e.find(a)->parent != 0 || !near3(T(a).position, ta0.position)) return fail("undo attach");

  // Attach snap, multi-select (a + c), one step.
  e.select(a);
  e.select(c, true);
  depth = e.undo_depth();
  if (!e.attach_selected(b, s3::AttachMode::SnapToTarget) || e.undo_depth() != depth + 1) return fail("snap step");
  for (std::uint64_t id : {a, c}) {
    if (e.find(id)->parent != b || !near3(T(id).position, T(b).position) || !sameq(T(id).rotation, T(b).rotation)) {
      return fail("snap-attach both");
    }
  }
  e.undo();
  if (e.find(c)->parent != 0 || near3(T(c).position, T(b).position)) return fail("undo snap-attach both");
  // A loop is refused.
  e.select(b);
  e.select(a, false);
  e.attach_selected(b, s3::AttachMode::KeepWorld);
  e.select(b);
  depth = e.undo_depth();
  if (e.attach_selected(a, s3::AttachMode::KeepWorld) || e.undo_depth() != depth) return fail("attach loop refused");
  e.undo();

  // Eyedropper: pick the action, click the target.
  e.select(c);
  e.begin_target_pick("attach_snap");
  if (!e.target_picking()) return fail("eyedropper armed");
  if (!e.finish_target_pick(b) || e.target_picking() || e.find(c)->parent != b) return fail("eyedropper attach");
  e.undo();
  e.begin_target_pick("snap");
  e.cancel_target_pick();
  if (e.target_picking()) return fail("eyedropper cancel");

  // Snap to object: moves onto it, no parent.
  e.select(c);
  depth = e.undo_depth();
  if (!e.snap_selected_to(b) || e.undo_depth() != depth + 1 || e.find(c)->parent != 0 ||
      !near3(T(c).position, T(b).position)) {
    return fail("snap to object");
  }
  e.undo();
  // Copy transform: position + scale only.
  r3::Transform tc = T(c);
  tc.scale = {1, 1, 1};
  e.set_transform(c, tc, "Reset");
  tb = T(b);
  tb.scale = {2, 2, 2};
  e.set_transform(b, tb, "Grow");
  e.select(c);
  depth = e.undo_depth();
  if (!e.copy_transform_to_selected(b, true, false, true) || e.undo_depth() != depth + 1) return fail("copy step");
  if (!near3(T(c).position, tb.position) || !near3(T(c).scale, {2, 2, 2}) || !sameq(T(c).rotation, tc.rotation)) {
    return fail("copy transform position + scale, rotation kept");
  }
  if (!e.copy_transform_to_selected(b, false, true, false) || !sameq(T(c).rotation, tb.rotation)) {
    return fail("copy transform rotation");
  }
  // Align to surface: a box above open ground lands on y = 0, upright.
  const std::uint64_t box = e.create_primitive("cube", {30.0f, 6.0f, 30.0f});
  r3::Transform tx = T(box);
  tx.rotation = r3::Quat::axis_angle({1, 0, 0}, r3::radians(20.0f));
  e.set_transform(box, tx, "Tilt");
  e.select(box);
  depth = e.undo_depth();
  if (!e.align_selected_to_surface() || e.undo_depth() != depth + 1) return fail("align to surface step");
  const r3::Aabb bb = e.world_bounds(*e.find(box));
  const r3::Vec3 up = T(box).rotation.rotate({0, 1, 0});
  if (!near(bb.min.y, 0.0f, 0.05f) || !near3(up, {0, 1, 0}, 1e-2f)) {
    return fail("align to surface: bottom " + std::to_string(bb.min.y) + " up " + f3(up));
  }
  // The helper on a slope: local +Y onto the normal.
  const r3::Vec3 n = r3::normalize(r3::Vec3{1, 1, 0});
  const r3::Transform sl = s3::align_to_surface(r3::Transform{}, {1, 2, 3}, n);
  if (!near3(sl.position, {1, 2, 3}) || !near3(sl.rotation.rotate({0, 1, 0}), n, 1e-3f)) return fail("slope normal");

  // Follow constraint: position only / rotation only / both, in Play.
  std::uint64_t rider = 0;
  for (const s3::Entity3D& x : e.scene().entities)
    if (x.player) rider = x.id;
  if (!rider) return fail("starter rider");
  const std::uint64_t fp = e.create_empty(T(rider).position + r3::Vec3{0, 3, 0});
  const std::uint64_t fr = e.create_empty({-8, 1, -8});
  const std::uint64_t fb = e.create_empty(T(rider).position + r3::Vec3{1, 0, 0});
  e.select(fp);
  if (!e.follow_selected(rider, true, false) || !e.find(fp)->follow) return fail("follow position step");
  e.select(fr);
  e.follow_selected(rider, false, true);
  e.select(fb);
  e.follow_selected(rider, true, true);
  const r3::Vec3 fr_pos = T(fr).position;
  if (!e.play_start()) return fail("play_start");
  w3::Input3D in;
  in.move_forward = 1.0f;
  in.turn = 1.5f;
  e.play_ticks(80, in);
  {
    w3::World3D& w = e.world();
    const r3::Transform rp = w.find(rider)->transform;
    const r3::Transform p = w.find(fp)->transform, rr = w.find(fr)->transform, bth = w.find(fb)->transform;
    if (!near3(p.position, rp.position + r3::Vec3{0, 3, 0}, 1e-2f)) return fail("follow position " + f3(p.position));
    if (!near3(rr.position, fr_pos, 1e-3f) || !sameq(rr.rotation, rp.rotation, 1e-2f)) return fail("follow rotation");
    const r3::Vec3 want = rp.position + rp.rotation.rotate({1, 0, 0});
    if (!near3(bth.position, want, 1e-2f) || !sameq(bth.rotation, rp.rotation, 1e-2f)) {
      return fail("follow both " + f3(bth.position) + " want " + f3(want));
    }
  }
  e.play_stop();
  e.select(fp);
  e.select(fr, true);
  depth = e.undo_depth();
  if (!e.remove_follow_selected() || e.undo_depth() != depth + 1 || e.find(fp)->follow || e.find(fr)->follow) {
    return fail("remove follow (multi)");
  }
  std::cout << "[smoke] attach 3D ok (keep / snap / multi / loop refused / detach / eyedropper / snap to / copy / "
               "align / follow pos, rot, both / undo)\n";
  return 0;
}

int bone3d() {
  const ed::ProjectInfo info = fresh_project("_attach_bone_smoke", ed::ProjectKind::ThreeD);
  std::string err;
  if (!s3::write_ghost_town_sample(info.path, "Bone Town", &err)) return fail(err);
  ed::Editor3DScreen e(info);
  e.on_enter();
  const s3::Entity3D* hank = e.scene().find_named("Old Hank");
  if (!hank || !hank->animator) return fail("Old Hank");
  const std::uint64_t hid = hank->id;
  const std::vector<std::string> bones = e.bone_names(hid);
  if (bones.size() < 5) return fail("bone_names");
  std::string bone;
  for (const std::string& b : bones)
    if (b.find("hand") != std::string::npos || b.find("lower_arm") != std::string::npos) bone = b;
  if (bone.empty()) bone = bones.back();
  const std::uint64_t lamp = e.create_empty({0, 0, 0});
  e.select(lamp);
  const std::size_t depth = e.undo_depth();
  if (!e.attach_selected_to_bone(hid, bone, s3::AttachMode::SnapToTarget) || e.undo_depth() != depth + 1) {
    return fail("attach to bone step");
  }
  if (e.find(lamp)->parent != hid || e.find(lamp)->parent_bone != bone) return fail("bone link");
  r3::Mat4 rest;
  const r3::Model* m = e.entity_model(*e.find(hid));
  if (!m || !s3::bone_world(*e.find(hid), *m, bone, nullptr, &rest)) return fail("bone_world rest");
  if (!near3(e.find(lamp)->transform.position, s3::bone_follow(*e.find(lamp), rest).position, 1e-3f)) {
    return fail("snapped onto the bone");
  }
  if (!e.play_start()) return fail("play_start");
  w3::World3D& w = e.world();
  const r3::Vec3 p0 = w.find(lamp)->transform.position;
  if (!w.script_play(hid, "wave", false)) return fail("wave");
  float moved = 0.0f, worst = 0.0f;
  for (int i = 0; i < 40; ++i) {
    w.step(w3::Input3D{});
    r3::AnimatorRuntime* a = w.animator(hid);
    r3::Mat4 bw;
    if (!a || !a->model() || !s3::bone_world(*w.find(hid), *a->model(), bone, &a->pose(), &bw)) return fail("pose");
    const r3::Transform want = s3::bone_follow(*w.find(lamp), bw);
    worst = std::max(worst, r3::length(want.position - w.find(lamp)->transform.position));
    moved = std::max(moved, r3::length(w.find(lamp)->transform.position - p0));
  }
  e.play_stop();
  if (worst > 1e-3f) return fail("bone follow off by " + std::to_string(worst));
  if (moved < 0.02f) return fail("the animated bone carried it (" + std::to_string(moved) + " m)");
  e.undo();
  if (e.find(lamp)->parent != 0 || !e.find(lamp)->parent_bone.empty()) return fail("undo bone attach");
  std::cout << "[smoke] attach bone ok (" << bone << ", wave moved it " << moved << " m, exact)\n";
  return 0;
}

// ---- 2D ---------------------------------------------------------------------

std::uint64_t make_box(ed::Editor2DScreen& scr, const char* name, float x, float y, float w, float h) {
  const std::uint64_t id = scr.create_entity(name);
  if (Entity2D* e = scr.workspace().find(id)) {
    e->x = x;
    e->y = y;
    e->w = w;
    e->h = h;
    e->rotation = 0.0f;
  }
  return id;
}

int attach2d() {
  ed::Editor2DScreen scr(fresh_project("_attach2d_smoke", ed::ProjectKind::TwoD));
  scr.on_enter();
  ed::Workspace2D& w = scr.workspace();
  w.set_snap_enabled(false);
  const std::uint64_t a = make_box(scr, "Lantern", 100, 100, 20, 20);
  const std::uint64_t b = make_box(scr, "Wagon", 300, 200, 100, 50);
  const std::uint64_t c = make_box(scr, "Barrel", -50, 0, 30, 30);
  auto E = [&](std::uint64_t i) { return *w.find(i); };
  // Keep.
  w.select(a);
  std::size_t depth = w.undo_count();
  if (!scr.attach_selected_2d(b, false) || w.undo_count() != depth + 1 || E(a).parent != b || !near(E(a).x, 100)) {
    return fail("2D attach keep");
  }
  // Detach keeps where it is.
  w.find(b)->x += 0;  // (no-op)
  if (!scr.detach_selected_2d() || E(a).parent != 0 || !near(E(a).x, 100)) return fail("2D detach");
  scr.undo();
  scr.undo();
  if (E(a).parent != 0) return fail("2D undo attach");
  // Snap, multi-select, one step: centres meet.
  w.select(a);
  w.toggle_selection(c);
  depth = w.undo_count();
  if (!scr.attach_selected_2d(b, true) || w.undo_count() != depth + 1) return fail("2D snap step");
  for (std::uint64_t id : {a, c}) {
    if (E(id).parent != b || !near(E(id).center_x(), E(b).center_x()) || !near(E(id).center_y(), E(b).center_y())) {
      return fail("2D snap-attach centres");
    }
  }
  scr.undo();
  // Eyedropper.
  w.select(c);
  scr.begin_target_pick_2d("attach_keep");
  if (!scr.target_picking_2d() || !scr.finish_target_pick_2d(b) || E(c).parent != b) return fail("2D eyedropper");
  scr.undo();
  // Snap to object (no parent) and copy transform.
  w.select(c);
  if (!scr.snap_selected_to_2d(b) || E(c).parent != 0 || !near(E(c).center_x(), E(b).center_x())) {
    return fail("2D snap to object");
  }
  scr.undo();
  w.find(b)->rotation = 30.0f;
  depth = w.undo_count();
  if (!scr.copy_transform_to_selected_2d(b, false, true, false) || w.undo_count() != depth + 1 ||
      !near(E(c).rotation, 30.0f) || !near(E(c).x, -50)) {
    return fail("2D copy rotation only");
  }
  if (!scr.copy_transform_to_selected_2d(b, false, false, true) || !near(E(c).w, 100) || !near(E(c).h, 50)) {
    return fail("2D copy scale");
  }
  // Align to surface: onto a static collider below.
  const std::uint64_t ledge = make_box(scr, "Ledge", -350, 400, 300, 20);
  w.find(ledge)->collider = ts::tombstone::ColliderData{};
  w.find(ledge)->collider->w = 300;
  w.find(ledge)->collider->h = 20;
  w.find(c)->x = -300.0f;  // clear of the starter gate, over the ledge only
  w.select(c);
  depth = w.undo_count();
  if (!scr.align_selected_to_surface_2d() || w.undo_count() != depth + 1) return fail("2D align step");
  if (!near(E(c).y + E(c).h, 400.0f) || !near(E(c).rotation, 0.0f)) return fail("2D align onto the ledge: " + std::to_string(E(c).y + E(c).h));
  // Follow in Play: position (offset kept) and rotation.
  std::uint64_t rider = 0;
  for (const Entity2D& x : w.entities())
    if (x.player) rider = x.id;
  w.select(a);
  if (!scr.follow_selected_2d(rider, true, false) || !E(a).follow) return fail("2D follow step");
  const float dx = E(a).center_x() - E(rider).center_x(), dy = E(a).center_y() - E(rider).center_y();
  rt::World world;
  std::string err;
  if (!world.build(w.entities(), "", &err)) return fail("2D world: " + err);
  rt::InputFrame right;
  right.slot(0) = rt::digital_input(false, true, false, false);
  for (int i = 0; i < 40; ++i) world.step(right);
  const Entity2D& pr = world.find(rider)->data;
  const Entity2D& pa = world.find(a)->data;
  if (pr.x < E(rider).x + 10.0f) return fail("2D rider moved");
  if (!near(pa.center_x() - pr.center_x(), dx, 0.05f) || !near(pa.center_y() - pr.center_y(), dy, 0.05f)) {
    return fail("2D follow keeps the offset");
  }
  if (!scr.remove_follow_selected_2d() || E(a).follow) return fail("2D remove follow");
  std::cout << "[smoke] attach 2D ok (keep / snap / multi / detach / eyedropper / snap to / copy / align / follow / "
               "undo)\n";
  return 0;
}

}  // namespace

int run_attach_smoke() {
  if (attach3d() != 0) return 1;
  if (bone3d() != 0) return 1;
  if (attach2d() != 0) return 1;
  std::cout << "[smoke] attach pass ok\n";
  return 0;
}
