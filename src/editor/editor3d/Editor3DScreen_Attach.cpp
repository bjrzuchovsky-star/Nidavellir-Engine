// 3D attach tools (docs/transforms.md "Attach"): Attach to... (keep the
// current position or snap to the target), Detach (world transform kept),
// the Follow constraint, Attach to bone (skinned models), Snap to object,
// Copy transform and Align to surface. Each works on every selected root,
// is one undo step and refuses while playing. Targets come from a picker
// list, the viewport eyedropper or a Hierarchy drag (Alt = snap).
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/ux/Actions.h"

#include "render3d/Gltf.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

using render3d::Vec3;
using scene3d::Entity3D;
using runtime::LogLevel;

namespace {

bool contains(const std::vector<std::uint64_t>& v, std::uint64_t id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}

}  // namespace

bool Editor3DScreen::attach_selected(std::uint64_t target, scene3d::AttachMode mode) {
  const Entity3D* t = scene_.find(target);
  if (playing() || !t) return false;
  const scene3d::Scene3D before = scene_;
  int done = 0;
  std::string why;
  for (std::uint64_t r : selection_roots()) {
    if (r == target) continue;
    const Entity3D* e = scene_.find(r);
    if (!e) continue;
    if (mode == scene3d::AttachMode::SnapToTarget && effectively_locked(*e)) continue;
    if (scene3d::attach(scene_.entities, r, target, mode, &why)) ++done;
  }
  if (done == 0) {
    if (!why.empty()) note("Attach: " + why, LogLevel::Warn);
    scene_ = before;
    return false;
  }
  const std::string what = done == 1 ? scene_.find(selection_roots().front() == target
                                                       ? selection_roots().back()
                                                       : selection_roots().front())->name
                                     : std::to_string(done) + " objects";
  push_undo((mode == scene3d::AttachMode::SnapToTarget ? "Snap-attach " : "Attach ") + what +
                " to " + scene_.find(target)->name,
            before);
  return true;
}

bool Editor3DScreen::detach_selected() {
  if (playing()) return false;
  const scene3d::Scene3D before = scene_;
  int done = 0;
  for (std::uint64_t id : selection()) {
    if (scene3d::detach(scene_.entities, id)) ++done;
  }
  if (done == 0) return false;
  push_undo(done == 1 ? "Detach " + scene_.find(selection().front())->name
                      : "Detach " + std::to_string(done) + " objects",
            before);
  return true;
}

bool Editor3DScreen::follow_selected(std::uint64_t target, bool position, bool rotation) {
  const Entity3D* t = scene_.find(target);
  if (playing() || !t || (!position && !rotation)) return false;
  const scene3d::Scene3D before = scene_;
  int done = 0;
  for (std::uint64_t r : selection_roots()) {
    Entity3D* e = scene_.find(r);
    if (!e || r == target) continue;
    // Following yourself through a parent chain would chase its own tail.
    if (scene3d::would_loop(scene_.entities, r, target)) continue;
    e->follow = scene3d::follow_from_current(e->transform, scene_.find(target)->transform,
                                             position, rotation, target);
    ++done;
  }
  if (done == 0) return false;
  const char* kind = position && rotation ? "Follow" : (position ? "Follow position of" : "Follow rotation of");
  push_undo(std::string(kind) + " " + scene_.find(target)->name, before);
  return true;
}

bool Editor3DScreen::remove_follow_selected() {
  if (playing()) return false;
  const scene3d::Scene3D before = scene_;
  int done = 0;
  for (std::uint64_t id : selection()) {
    Entity3D* e = scene_.find(id);
    if (e && e->follow) {
      e->follow.reset();
      ++done;
    }
  }
  if (done == 0) return false;
  push_undo("Remove Follow", before);
  return true;
}

std::vector<std::string> Editor3DScreen::bone_names(std::uint64_t target) {
  std::vector<std::string> out;
  const Entity3D* e = scene_.find(target);
  if (!e) return out;
  const render3d::Model* m = entity_model(*e);
  if (!m) return out;
  for (const render3d::Joint& j : m->skeleton.joints) out.push_back(j.name);
  return out;
}

bool Editor3DScreen::attach_selected_to_bone(std::uint64_t target, const std::string& bone,
                                             scene3d::AttachMode mode) {
  const Entity3D* t = scene_.find(target);
  if (playing() || !t) return false;
  const render3d::Model* m = entity_model(*t);
  render3d::Mat4 bw;
  if (!m || !scene3d::bone_world(*t, *m, bone, nullptr, &bw)) {
    note("Attach to bone: " + t->name + " has no bone \"" + bone + "\"", LogLevel::Warn);
    return false;
  }
  const scene3d::Scene3D before = scene_;
  int done = 0;
  std::string why;
  for (std::uint64_t r : selection_roots()) {
    if (r == target) continue;
    if (scene3d::attach_to_bone(scene_.entities, r, target, bone, bw, mode, &why)) ++done;
  }
  if (done == 0) {
    if (!why.empty()) note("Attach to bone: " + why, LogLevel::Warn);
    scene_ = before;
    return false;
  }
  push_undo("Attach to " + scene_.find(target)->name + " / " + bone, before);
  return true;
}

bool Editor3DScreen::snap_selected_to(std::uint64_t target) {
  const Entity3D* t = scene_.find(target);
  if (playing() || !t) return false;
  const Vec3 at = t->transform.position;
  const scene3d::Scene3D before = scene_;
  int done = 0;
  for (std::uint64_t r : selection_roots()) {
    const Entity3D* e = scene_.find(r);
    if (!e || r == target || effectively_locked(*e)) continue;
    render3d::Transform now = e->transform;
    now.position = at;
    scene3d::move_with_children(scene_.entities, r, now);
    ++done;
  }
  if (done == 0) return false;
  push_undo("Snap to " + scene_.find(target)->name, before);
  return true;
}

bool Editor3DScreen::copy_transform_to_selected(std::uint64_t from, bool position, bool rotation,
                                                bool scale) {
  const Entity3D* f = scene_.find(from);
  if (playing() || !f || (!position && !rotation && !scale)) return false;
  const render3d::Transform src = f->transform;
  const scene3d::Scene3D before = scene_;
  int done = 0;
  for (std::uint64_t r : selection_roots()) {
    const Entity3D* e = scene_.find(r);
    if (!e || r == from || effectively_locked(*e)) continue;
    scene3d::move_with_children(scene_.entities, r,
                                scene3d::copy_transform(src, e->transform, position, rotation, scale));
    ++done;
  }
  if (done == 0 || before == scene_) {
    scene_ = before;
    return false;
  }
  push_undo("Copy transform from " + scene_.find(from)->name, before);
  return true;
}

bool Editor3DScreen::ray_surface(const render3d::Ray& ray, const std::vector<std::uint64_t>& skip,
                                 float max_t, float* t_out) {
  float best = max_t;
  bool hit = false;
  float t = 0.0f;
  std::uint64_t tid = 0;
  if (terrain_ray(ray, &t, &tid) && t < best && !contains(skip, tid)) {
    best = t;
    hit = true;
  }
  for (const Entity3D& e : scene_.entities) {
    if (!e.visible || effectively_hidden(e) || e.terrain || !e.mesh || contains(skip, e.id)) continue;
    const render3d::Aabb local = view_ ? view_->local_bounds(e) : render3d::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    if (render3d::ray_obb(ray, local, e.transform.matrix(), &t) && t > 1e-4f && t < best) {
      best = t;
      hit = true;
    }
  }
  if (std::fabs(ray.dir.y) > 1e-6f) {
    // The ground plane (y = 0) when nothing else is below.
    const float tp = -ray.origin.y / ray.dir.y;
    if (tp > 1e-4f && tp < best) {
      best = tp;
      hit = true;
    }
  }
  if (hit) *t_out = best;
  return hit;
}

bool Editor3DScreen::align_selected_to_surface() {
  if (playing()) return false;
  const scene3d::Scene3D before = scene_;
  int done = 0;
  for (std::uint64_t r : selection_roots()) {
    const Entity3D* e = scene_.find(r);
    if (!e || effectively_locked(*e)) continue;
    std::vector<std::uint64_t> skip = scene3d::descendants_of(scene_.entities, r);
    skip.push_back(r);
    const Vec3 p = e->transform.position;
    // Three rays straight down (centre, +X, +Z) give the hit and the
    // surface normal, whatever is below (mesh box, terrain, ground).
    const float step = 0.15f;
    Vec3 pts[3];
    bool ok = true;
    const Vec3 offs[3] = {{0, 0, 0}, {step, 0, 0}, {0, 0, step}};
    for (int i = 0; i < 3 && ok; ++i) {
      const render3d::Ray ray{p + offs[i] + Vec3{0, 0.5f, 0}, {0, -1, 0}};
      float t = 0.0f;
      ok = ray_surface(ray, skip, 2000.0f, &t);
      if (ok) pts[i] = ray.origin + ray.dir * t;
    }
    if (!ok) continue;
    Vec3 n = render3d::normalize(render3d::cross(pts[2] - pts[0], pts[1] - pts[0]));
    if (n.y < 0) n = n * -1.0f;
    if (!(n.y > 0.05f)) n = {0, 1, 0};
    // Rest the object's bottom on the surface: lift by how far its origin
    // sits above its lowest point once it has the new rotation.
    render3d::Transform now = scene3d::align_to_surface(e->transform, pts[0], n);
    Entity3D turned = *e;
    turned.transform.rotation = now.rotation;
    const render3d::Aabb wb = world_bounds(turned);
    const float lift = std::max(0.0f, p.y - wb.min.y);
    now.position = pts[0] + n * lift;
    scene3d::move_with_children(scene_.entities, r, now);
    ++done;
  }
  if (done == 0 || before == scene_) {
    scene_ = before;
    return false;
  }
  push_undo(done == 1 ? "Align " + scene_.find(selection_roots().front())->name + " to surface"
                      : "Align " + std::to_string(done) + " objects to surface",
            before);
  return true;
}

// ---- Eyedropper --------------------------------------------------------------------

void Editor3DScreen::begin_target_pick(const std::string& action) {
  if (playing() || selection().empty()) return;
  pick_action_ = action;
  note("Click the target in the viewport or Hierarchy (Esc cancels)");
}

bool Editor3DScreen::finish_target_pick(std::uint64_t target) {
  const std::string a = pick_action_;
  pick_action_.clear();
  if (a.empty() || !scene_.find(target)) return false;
  using M = scene3d::AttachMode;
  if (a == "attach_keep") return attach_selected(target, M::KeepWorld);
  if (a == "attach_snap") return attach_selected(target, M::SnapToTarget);
  if (a == "follow_pos") return follow_selected(target, true, false);
  if (a == "follow_rot") return follow_selected(target, false, true);
  if (a == "follow_both") return follow_selected(target, true, true);
  if (a == "snap") return snap_selected_to(target);
  if (a == "copy") return copy_transform_to_selected(target, copy_pos_, copy_rot_, copy_scale_);
  if (a == "bone_keep" || a == "bone_snap") {
    // A bone needs a list: open the context menu's bone picker on it.
    const std::vector<std::string> bones = bone_names(target);
    if (bones.empty()) {
      note("Attach to bone: " + scene_.find(target)->name + " has no skeleton", LogLevel::Warn);
      return false;
    }
    note("Pick the bone: right-click > Attach to bone > " + scene_.find(target)->name);
    return false;
  }
  return false;
}

// ---- Menus -------------------------------------------------------------------------

bool Editor3DScreen::attach_target_list(const char* id, std::uint64_t* out) {
  ImGui::PushID(id);
  const std::vector<std::uint64_t>& sel = selection();
  bool picked = false;
  int shown = 0;
  for (const Entity3D& e : scene_.entities) {
    if (contains(sel, e.id)) continue;
    ++shown;
    ImGui::PushID(static_cast<int>(e.id));
    if (ux::MenuItem(e.name.c_str())) {
      *out = e.id;
      picked = true;
    }
    ImGui::PopID();
    if (shown > 400) break;
  }
  if (shown == 0) ImGui::TextDisabled("(nothing else in the scene)");
  ImGui::PopID();
  return picked;
}

void Editor3DScreen::draw_attach_menu(std::uint64_t clicked) {
  (void)clicked;
  const bool has = !selection().empty() && !playing();
  using M = scene3d::AttachMode;
  std::uint64_t t = 0;
  if (ux::BeginMenu("Attach to...", has)) {
    if (ux::BeginMenu("Keep current position")) {
      if (ux::MenuItem("Pick in viewport (eyedropper)")) begin_target_pick("attach_keep");
      ImGui::Separator();
      if (attach_target_list("keep", &t)) attach_selected(t, M::KeepWorld);
      ux::EndMenu();
    }
    if (ux::BeginMenu("Snap to target")) {
      if (ux::MenuItem("Pick in viewport (eyedropper)")) begin_target_pick("attach_snap");
      ImGui::Separator();
      if (attach_target_list("snap", &t)) attach_selected(t, M::SnapToTarget);
      ux::EndMenu();
    }
    ImGui::TextDisabled("Hierarchy: drag onto a row = keep, Alt+drag = snap");
    ux::EndMenu();
  }
  if (ux::BeginMenu("Attach to bone...", has)) {
    int skinned = 0;
    for (const Entity3D& e : scene_.entities) {
      if (contains(selection(), e.id) || !e.mesh) continue;
      const render3d::Model* m = entity_model(e);
      if (!m || m->skeleton.joints.empty()) continue;
      ++skinned;
      ImGui::PushID(static_cast<int>(e.id));
      if (ux::BeginMenu(e.name.c_str())) {
        static bool snap = false;
        ux::BeginMenuOptions();
        ImGui::Checkbox("Snap to bone", &snap);
        ux::EndMenuOptions();
        ImGui::Separator();
        for (const render3d::Joint& j : m->skeleton.joints) {
          if (ux::MenuItem(j.name.c_str())) {
            attach_selected_to_bone(e.id, j.name, snap ? M::SnapToTarget : M::KeepWorld);
          }
        }
        ux::EndMenu();
      }
      ImGui::PopID();
    }
    if (skinned == 0) ImGui::TextDisabled("(no skinned models in the scene)");
    ux::EndMenu();
  }
  bool any_parent = false, any_follow = false;
  for (std::uint64_t id : selection()) {
    if (const Entity3D* e = scene_.find(id)) {
      any_parent |= e->parent != 0;
      any_follow |= e->follow.has_value();
    }
  }
  if (ux::MenuItem("Detach", nullptr, false, has && any_parent)) detach_selected();
  if (ux::BeginMenu("Follow...", has)) {
    struct K { const char* label; const char* action; bool p, r; };
    for (const K k : {K{"Position and rotation", "follow_both", true, true},
                      K{"Position only", "follow_pos", true, false},
                      K{"Rotation only", "follow_rot", false, true}}) {
      if (ux::BeginMenu(k.label)) {
        if (ux::MenuItem("Pick in viewport (eyedropper)")) begin_target_pick(k.action);
        ImGui::Separator();
        if (attach_target_list(k.action, &t)) follow_selected(t, k.p, k.r);
        ux::EndMenu();
      }
    }
    ux::EndMenu();
  }
  if (any_follow && ux::MenuItem("Remove Follow", nullptr, false, has)) remove_follow_selected();
  ImGui::Separator();
  if (ux::BeginMenu("Snap to object", has)) {
    if (ux::MenuItem("Pick in viewport (eyedropper)")) begin_target_pick("snap");
    ImGui::Separator();
    if (attach_target_list("snapto", &t)) snap_selected_to(t);
    ux::EndMenu();
  }
  if (ux::BeginMenu("Copy transform from", has)) {
    ux::BeginMenuOptions();
    ImGui::Checkbox("Position", &copy_pos_);
    ImGui::SameLine();
    ImGui::Checkbox("Rotation", &copy_rot_);
    ImGui::SameLine();
    ImGui::Checkbox("Scale", &copy_scale_);
    ux::EndMenuOptions();
    if (ux::MenuItem("Pick in viewport (eyedropper)")) begin_target_pick("copy");
    ImGui::Separator();
    if (attach_target_list("copy", &t)) copy_transform_to_selected(t, copy_pos_, copy_rot_, copy_scale_);
    ux::EndMenu();
  }
  if (ux::MenuItem("Align to surface", nullptr, false, has)) align_selected_to_surface();
}

void Editor3DScreen::draw_object_menu() {
  const bool has = !selection().empty() && !playing();
  if (ux::MenuItem("Create camera", nullptr, false, !playing())) create_camera();
  const Entity3D* sel = scene_.find(primary());
  const bool cam = sel && sel->camera;
  if (ux::MenuItem("Make active camera", nullptr, false, cam && !playing())) make_active_camera(sel->id);
  if (ux::MenuItem("Align camera to view", "Ctrl+Alt+Num0", false, !playing() && (cam || play_camera_id())))
    align_camera_to_view();
  if (ux::MenuItem(looking_through() ? "Stop looking through camera" : "Look through camera", "Num0", false,
                      cam || play_camera_id() || looking_through())) {
    if (looking_through()) stop_looking_through();
    else look_through_camera();
  }
  ImGui::Separator();
  draw_attach_menu(primary());
  ImGui::Separator();
  if (sel && ux::MenuItem(sel->locked ? "Unlock" : "Lock", "Ctrl+L", false, has)) set_locked(sel->id, !sel->locked);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
