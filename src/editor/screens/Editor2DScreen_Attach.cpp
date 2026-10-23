// 2D editor cameras + attach tools (docs/camera.md, docs/transforms.md):
// Object menu, the Attach to... / Follow / Snap / Copy transform / Align to
// surface entries of the context menus, the eyedropper, camera creation and
// "make active", and the v13 camera migration on open. Every edit is one
// undo step over the selection roots.
#include "editor/screens/Editor2DScreen.h"
#include "editor/ux/Actions.h"

#include "editor/workspace/SceneIO.h"
#include "scene/CameraMigrate.h"

#include <imgui.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

bool contains(const std::vector<std::uint64_t>& v, std::uint64_t id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}

}  // namespace

void Editor2DScreen::migrate_cameras_on_open_2d(const std::vector<std::string>& lines) {
  camera_backup2d_.clear();
  if (lines.empty()) return;
  std::string err;
  const std::string rel = camera_migrate::backup_scene(project_.path, scene_path_, &err);
  if (rel.empty()) {
    // No backup: keep the file as it was (the runtime still plays it with
    // the implicit rider camera); the editor shows the migrated copy.
    note("Camera migration: could not back up scene.json (" + err + "); not saved");
    return;
  }
  camera_backup2d_ = rel;
  std::vector<std::string> log{"scene.json (2D): cameras (v13), backup " + rel};
  for (const std::string& l : lines) log.push_back("  " + l);
  camera_migrate::append_log(project_.path, log);
  for (const std::string& l : lines) std::cout << "[Editor2D] camera migration: " << l << '\n';
  std::string save_err;
  if (!scene_io::save(workspace_, scene_path_, &save_err)) {
    note("Camera migration could not save scene.json: " + save_err);
    return;
  }
  note("The rider camera is now a scene object (backup: " + rel + ")");
}

std::uint64_t Editor2DScreen::create_camera_2d() {
  if (is_playing()) return 0;
  cancel_rename();
  Workspace2D::Snapshot before = prepare_edit();
  std::string name = "Camera";
  for (int n = 2; std::any_of(workspace_.entities().begin(), workspace_.entities().end(),
                              [&](const Entity2D& e) { return e.name == name; });
       ++n) {
    name = "Camera " + std::to_string(n);
  }
  const std::uint64_t id = workspace_.create_entity(name);
  Entity2D* e = workspace_.find(id);
  if (!e) return 0;
  e->w = 48.0f;
  e->h = 32.0f;
  e->x = workspace_.pan_x() - e->w * 0.5f;
  e->y = workspace_.pan_y() - e->h * 0.5f;
  e->color[0] = 0.95f;
  e->color[1] = 0.75f;
  e->color[2] = 0.25f;
  e->color[3] = 1.0f;
  Camera2DData c;
  for (const Entity2D& other : workspace_.entities()) {
    if (other.player && other.player->slot == 0) {
      c.target = other.id;
      break;
    }
  }
  // Priority 0: the camera already playing keeps playing (ties go by scene
  // order) until this one is made active.
  e->camera = c;
  workspace_.select(id);
  commit_discrete(std::move(before), "Create Camera", "Camera staked");
  return id;
}

std::uint64_t Editor2DScreen::play_camera_2d() const {
  std::uint64_t best = 0;
  int best_p = 0;
  for (const Entity2D& e : workspace_.entities()) {
    if (!e.camera || !e.camera->active) continue;
    if (best == 0 || e.camera->priority > best_p) {
      best = e.id;
      best_p = e.camera->priority;
    }
  }
  return best;
}

bool Editor2DScreen::make_active_camera_2d(std::uint64_t id) {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  Entity2D* e = workspace_.find(id);
  if (!e || !e->camera) return false;
  int top = e->camera->priority;
  for (const Entity2D& o : workspace_.entities()) {
    if (o.id != id && o.camera && o.camera->active) top = std::max(top, o.camera->priority + 1);
  }
  e = workspace_.find(id);
  e->camera->active = true;
  e->camera->priority = top;
  return commit_discrete(std::move(before), "Make Active Camera", e->name + " is the Play camera");
}

bool Editor2DScreen::attach_selected_2d(std::uint64_t target, bool snap) {
  if (is_playing() || !workspace_.find(target)) return false;
  Workspace2D::Snapshot before = prepare_edit();
  int done = 0;
  for (std::uint64_t r : workspace_.selection_roots()) {
    if (r == target) continue;
    const Entity2D* e = workspace_.find(r);
    if (!e || (snap && workspace_.effectively_locked(*e))) continue;
    if (workspace_.attach_to(r, target, snap)) ++done;
  }
  if (done == 0) return false;
  const std::string tn = workspace_.find(target)->name;
  return commit_discrete(std::move(before), snap ? "Snap-attach" : "Attach",
                         (done == 1 ? std::string("Attached to ") : std::to_string(done) + " attached to ") + tn);
}

bool Editor2DScreen::detach_selected_2d() {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  int done = 0;
  for (std::uint64_t id : workspace_.selection()) {
    const Entity2D* e = workspace_.find(id);
    if (e && e->parent != 0 && workspace_.set_parent(id, 0)) ++done;
  }
  if (done == 0) return false;
  return commit_discrete(std::move(before), "Detach", "Detached (positions kept)");
}

bool Editor2DScreen::follow_selected_2d(std::uint64_t target, bool position, bool rotation) {
  if (is_playing() || !workspace_.find(target)) return false;
  Workspace2D::Snapshot before = prepare_edit();
  int done = 0;
  for (std::uint64_t r : workspace_.selection_roots()) {
    if (r == target || workspace_.is_ancestor(r, target)) continue;
    if (workspace_.set_follow(r, target, position, rotation)) ++done;
  }
  if (done == 0) return false;
  return commit_discrete(std::move(before), "Follow", "Following " + workspace_.find(target)->name);
}

bool Editor2DScreen::remove_follow_selected_2d() {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  int done = 0;
  for (std::uint64_t id : workspace_.selection()) {
    Entity2D* e = workspace_.find(id);
    if (e && e->follow) {
      e->follow.reset();
      ++done;
    }
  }
  if (done == 0) return false;
  return commit_discrete(std::move(before), "Remove Follow", "Follow removed");
}

bool Editor2DScreen::snap_selected_to_2d(std::uint64_t target) {
  if (is_playing() || !workspace_.find(target)) return false;
  Workspace2D::Snapshot before = prepare_edit();
  int done = 0;
  for (std::uint64_t r : workspace_.selection_roots()) {
    const Entity2D* e = workspace_.find(r);
    if (!e || r == target || workspace_.effectively_locked(*e)) continue;
    if (workspace_.snap_to(r, target)) ++done;
  }
  if (done == 0) return false;
  return commit_discrete(std::move(before), "Snap to Object", "Snapped to " + workspace_.find(target)->name);
}

bool Editor2DScreen::copy_transform_to_selected_2d(std::uint64_t from, bool position, bool rotation,
                                                   bool scale) {
  if (is_playing() || !workspace_.find(from)) return false;
  Workspace2D::Snapshot before = prepare_edit();
  int done = 0;
  for (std::uint64_t r : workspace_.selection_roots()) {
    const Entity2D* e = workspace_.find(r);
    if (!e || r == from || workspace_.effectively_locked(*e)) continue;
    if (workspace_.copy_transform(from, r, position, rotation, scale)) ++done;
  }
  if (done == 0) return false;
  return commit_discrete(std::move(before), "Copy Transform", "Transform copied from " + workspace_.find(from)->name);
}

bool Editor2DScreen::align_selected_to_surface_2d() {
  if (is_playing()) return false;
  Workspace2D::Snapshot before = prepare_edit();
  int done = 0;
  for (std::uint64_t r : workspace_.selection_roots()) {
    const Entity2D* e = workspace_.find(r);
    if (!e || workspace_.effectively_locked(*e)) continue;
    if (workspace_.align_to_surface(r)) ++done;
  }
  if (done == 0) {
    note("Align to surface: no solid tile or collider below");
    return false;
  }
  return commit_discrete(std::move(before), "Align to Surface", "Set down on the surface below");
}

void Editor2DScreen::begin_target_pick_2d(const std::string& action) {
  if (is_playing() || workspace_.selection_count() == 0) return;
  pick_action2d_ = action;
  note("Click the target in the viewport or Hierarchy (Esc cancels)");
}

bool Editor2DScreen::finish_target_pick_2d(std::uint64_t target) {
  const std::string a = pick_action2d_;
  pick_action2d_.clear();
  if (a.empty() || !workspace_.find(target)) return false;
  if (a == "attach_keep") return attach_selected_2d(target, false);
  if (a == "attach_snap") return attach_selected_2d(target, true);
  if (a == "follow_pos") return follow_selected_2d(target, true, false);
  if (a == "follow_rot") return follow_selected_2d(target, false, true);
  if (a == "follow_both") return follow_selected_2d(target, true, true);
  if (a == "snap") return snap_selected_to_2d(target);
  if (a == "copy") return copy_transform_to_selected_2d(target, copy_pos2d_, copy_rot2d_, copy_scale2d_);
  return false;
}

bool Editor2DScreen::attach_target_list_2d(const char* id, std::uint64_t* out) {
  ImGui::PushID(id);
  bool picked = false;
  int shown = 0;
  for (const Entity2D& e : workspace_.entities()) {
    if (workspace_.is_selected(e.id)) continue;
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

void Editor2DScreen::draw_attach_menu_2d() {
  const bool has = workspace_.selection_count() > 0 && !is_playing();
  std::uint64_t t = 0;
  if (ux::BeginMenu("Attach to...", has)) {
    if (ux::BeginMenu("Keep current position")) {
      if (ux::MenuItem("Pick in viewport (eyedropper)")) begin_target_pick_2d("attach_keep");
      ImGui::Separator();
      if (attach_target_list_2d("keep", &t)) attach_selected_2d(t, false);
      ux::EndMenu();
    }
    if (ux::BeginMenu("Snap to target")) {
      if (ux::MenuItem("Pick in viewport (eyedropper)")) begin_target_pick_2d("attach_snap");
      ImGui::Separator();
      if (attach_target_list_2d("snap", &t)) attach_selected_2d(t, true);
      ux::EndMenu();
    }
    ImGui::TextDisabled("Hierarchy: drag onto a row = keep, Alt+drag = snap");
    ux::EndMenu();
  }
  bool any_parent = false, any_follow = false;
  for (std::uint64_t id : workspace_.selection()) {
    if (const Entity2D* e = workspace_.find(id)) {
      any_parent |= e->parent != 0;
      any_follow |= e->follow.has_value();
    }
  }
  if (ux::MenuItem("Detach", nullptr, false, has && any_parent)) detach_selected_2d();
  if (ux::BeginMenu("Follow...", has)) {
    struct K { const char* label; const char* action; bool p, r; };
    for (const K k : {K{"Position and rotation", "follow_both", true, true},
                      K{"Position only", "follow_pos", true, false},
                      K{"Rotation only", "follow_rot", false, true}}) {
      if (ux::BeginMenu(k.label)) {
        if (ux::MenuItem("Pick in viewport (eyedropper)")) begin_target_pick_2d(k.action);
        ImGui::Separator();
        if (attach_target_list_2d(k.action, &t)) follow_selected_2d(t, k.p, k.r);
        ux::EndMenu();
      }
    }
    ux::EndMenu();
  }
  if (any_follow && ux::MenuItem("Remove Follow", nullptr, false, has)) remove_follow_selected_2d();
  ImGui::Separator();
  if (ux::BeginMenu("Snap to object", has)) {
    if (ux::MenuItem("Pick in viewport (eyedropper)")) begin_target_pick_2d("snap");
    ImGui::Separator();
    if (attach_target_list_2d("snapto", &t)) snap_selected_to_2d(t);
    ux::EndMenu();
  }
  if (ux::BeginMenu("Copy transform from", has)) {
    ux::BeginMenuOptions();
    ImGui::Checkbox("Position", &copy_pos2d_);
    ImGui::SameLine();
    ImGui::Checkbox("Rotation", &copy_rot2d_);
    ImGui::SameLine();
    ImGui::Checkbox("Scale", &copy_scale2d_);
    ux::EndMenuOptions();
    if (ux::MenuItem("Pick in viewport (eyedropper)")) begin_target_pick_2d("copy");
    ImGui::Separator();
    if (attach_target_list_2d("copy", &t)) copy_transform_to_selected_2d(t, copy_pos2d_, copy_rot2d_, copy_scale2d_);
    ux::EndMenu();
  }
  if (ux::MenuItem("Align to surface", nullptr, false, has)) align_selected_to_surface_2d();
}

void Editor2DScreen::draw_inspector_follow_2d(std::uint64_t id) {
  const Entity2D* e = workspace_.find(id);
  if (!e || !e->follow) return;
  ImGui::PushID("follow2d");
  ImGui::SeparatorText("Follow");
  const Entity2D* tgt = workspace_.find(e->follow->target);
  ImGui::Text("Target: %s", tgt ? tgt->name.c_str() : "(missing)");
  bool pos = e->follow->position;
  bool rot = e->follow->rotation;
  bool remove = false;
  auto edit = [&](const char* label, auto&& fn) {
    Workspace2D::Snapshot before = prepare_edit();
    if (Entity2D* t = workspace_.find(id); t && t->follow) {
      fn(*t);
      commit_discrete(std::move(before), label, "");
    }
  };
  if (ImGui::Checkbox("Position", &pos)) edit("Follow Position", [&](Entity2D& t) { t.follow->position = pos; });
  ImGui::SameLine();
  if (ImGui::Checkbox("Rotation", &rot)) edit("Follow Rotation", [&](Entity2D& t) { t.follow->rotation = rot; });
  if (Entity2D* t = workspace_.find(id); t && t->follow) {
    if (ImGui::DragFloat2("Offset", &t->follow->offset_x, 0.5f)) mark_dirty();
    track_inspector_item("Edit Follow Offset");
    if (ImGui::DragFloat("Rotation offset", &t->follow->rotation_offset, 0.5f)) mark_dirty();
    track_inspector_item("Edit Follow Rotation Offset");
  }
  if (ImGui::SmallButton("Remove Follow")) remove = true;
  ImGui::PopID();
  if (remove) {
    Workspace2D::Snapshot before = prepare_edit();
    if (Entity2D* t = workspace_.find(id)) t->follow.reset();
    commit_discrete(std::move(before), "Remove Follow", "Follow removed");
  }
}

void Editor2DScreen::draw_object_menu_2d() {
  if (ux::MenuItem("Create camera", nullptr, false, !is_playing())) create_camera_2d();
  const Entity2D* sel = workspace_.selected();
  const bool cam = sel && sel->camera;
  if (ux::MenuItem("Make active camera", nullptr, false, cam && !is_playing())) make_active_camera_2d(sel->id);
  ImGui::Separator();
  draw_attach_menu_2d();
  ImGui::Separator();
  if (sel && ux::MenuItem(sel->locked ? "Unlock" : "Lock", "Ctrl+L", false, !is_playing()))
    lock_selected(!sel->locked);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
