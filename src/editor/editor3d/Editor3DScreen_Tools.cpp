// 3D editor tools: parenting (children ride along), clipboard JSON,
// group / ungroup, lock / hide, align / distribute, drop to ground,
// surface snapping and Alt+drag copies. Every edit is one undo step and
// autosaves scene.json (v9); all refuse while playing.
#include "editor/editor3d/Editor3DScreen.h"

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

namespace {

std::string& clipboard_store() {
  static std::string text;
  return text;
}

bool contains(const std::vector<std::uint64_t>& v, std::uint64_t id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}

std::string unique_name(const scene3d::Scene3D& s, const std::string& base) {
  if (!s.find_named(base)) return base;
  std::string name = base + " copy";
  for (int n = 2; s.find_named(name); ++n) name = base + " copy " + std::to_string(n);
  return name;
}

}  // namespace

// ---- Tree queries ---------------------------------------------------------------

std::vector<std::uint64_t> Editor3DScreen::children_of(std::uint64_t id) const {
  std::vector<std::uint64_t> out;
  for (const Entity3D& e : scene_.entities) {
    if (e.parent == id && e.id != id) out.push_back(e.id);
  }
  return out;
}

std::vector<std::uint64_t> Editor3DScreen::selection_roots() const {
  std::vector<std::uint64_t> roots;
  for (std::uint64_t id : scene_.selection) {
    const Entity3D* e = scene_.find(id);
    bool covered = false;
    std::size_t guard = 0;
    while (e && e->parent != 0 && guard++ <= scene_.entities.size()) {
      if (contains(scene_.selection, e->parent)) {
        covered = true;
        break;
      }
      e = scene_.find(e->parent);
    }
    if (!covered && !contains(roots, id)) roots.push_back(id);
  }
  return roots;
}

bool Editor3DScreen::effectively_hidden(const Entity3D& e) const {
  const Entity3D* cur = &e;
  std::size_t guard = 0;
  while (cur && guard++ <= scene_.entities.size()) {
    if (cur->editor_hidden) return true;
    cur = cur->parent ? scene_.find(cur->parent) : nullptr;
  }
  return false;
}

bool Editor3DScreen::effectively_locked(const Entity3D& e) const {
  const Entity3D* cur = &e;
  std::size_t guard = 0;
  while (cur && guard++ <= scene_.entities.size()) {
    if (cur->locked) return true;
    cur = cur->parent ? scene_.find(cur->parent) : nullptr;
  }
  return false;
}

render3d::Aabb Editor3DScreen::world_bounds(const Entity3D& e) {
  const render3d::Aabb local =
      view_ ? view_->local_bounds(e) : render3d::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
  return local.transformed(e.transform.matrix());
}

void Editor3DScreen::carry_descendants(std::uint64_t id, const render3d::Transform& prev) {
  const Entity3D* p = scene_.find(id);
  if (!p || p->transform == prev) return;
  const render3d::Transform now = p->transform;
  for (std::uint64_t d : scene3d::descendants_of(scene_.entities, id)) {
    if (Entity3D* c = scene_.find(d)) scene3d::carry_child(c, prev, now);
  }
}

// ---- Duplicate / clipboard ----------------------------------------------------------

std::size_t Editor3DScreen::duplicate_roots(bool offset) {
  const std::vector<std::uint64_t> roots = selection_roots();
  std::vector<std::uint64_t> fresh;
  const bool had_player = std::any_of(scene_.entities.begin(), scene_.entities.end(),
                                      [](const Entity3D& e) { return e.player.has_value(); });
  for (std::uint64_t r : roots) {
    std::vector<std::uint64_t> block{r};
    for (std::uint64_t d : scene3d::descendants_of(scene_.entities, r)) block.push_back(d);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> remap;
    std::vector<Entity3D> copies;
    for (std::uint64_t id : block) {
      const Entity3D* src = scene_.find(id);
      if (!src) continue;
      Entity3D e = *src;
      e.id = scene_.next_id() + copies.size();
      remap.emplace_back(id, e.id);
      if (offset) e.transform.position.x += 1.0f;
      if (had_player && e.player) e.player.reset();  // one rider
      if (e.vr_rig) e.vr_rig.reset();                 // one headset rig
      e.locked = false;
      copies.push_back(std::move(e));
    }
    for (Entity3D& e : copies) {
      if (e.parent != 0) {
        for (const auto& m : remap) {
          if (m.first == e.parent && e.id != remap.front().second) e.parent = m.second;
        }
      }
      std::string name = unique_name(scene_, e.name);
      e.name = name;
      scene_.entities.push_back(e);
    }
    if (!remap.empty()) fresh.push_back(remap.front().second);
  }
  scene_.selection = fresh;
  return fresh.size();
}

std::string Editor3DScreen::copy_json() const {
  const std::vector<std::uint64_t> roots = selection_roots();
  if (roots.empty()) return {};
  std::vector<std::uint64_t> keep;
  for (std::uint64_t r : roots) {
    keep.push_back(r);
    for (std::uint64_t d : scene3d::descendants_of(scene_.entities, r)) keep.push_back(d);
  }
  scene3d::Scene3D clip;
  clip.environment = scene_.environment;
  for (const Entity3D& e : scene_.entities) {
    if (!contains(keep, e.id)) continue;
    Entity3D c = e;
    if (contains(roots, c.id)) c.parent = 0;
    clip.entities.push_back(std::move(c));
  }
  return scene3d::write(clip);
}

std::size_t Editor3DScreen::paste_json(const std::string& text, bool offset) {
  if (playing()) return 0;
  scene3d::Scene3D clip;
  std::string err;
  if (!scene3d::is_3d_scene_text(text) || !scene3d::parse(text, &clip, &err) ||
      clip.entities.empty()) {
    return 0;
  }
  const scene3d::Scene3D before = scene_;
  const bool had_player = std::any_of(scene_.entities.begin(), scene_.entities.end(),
                                      [](const Entity3D& e) { return e.player.has_value(); });
  const bool had_rig = std::any_of(scene_.entities.begin(), scene_.entities.end(),
                                   [](const Entity3D& e) { return e.vr_rig.has_value(); });
  std::vector<std::pair<std::uint64_t, std::uint64_t>> remap;
  std::uint64_t next = scene_.next_id();
  for (const Entity3D& e : clip.entities) remap.emplace_back(e.id, next++);
  auto mapped = [&](std::uint64_t id) -> std::uint64_t {
    for (const auto& m : remap) {
      if (m.first == id) return m.second;
    }
    return 0;
  };
  std::vector<std::uint64_t> roots;
  for (Entity3D e : clip.entities) {
    e.id = mapped(e.id);
    e.parent = e.parent ? mapped(e.parent) : 0;
    if (e.parent == 0) roots.push_back(e.id);
    if (offset) e.transform.position.x += 1.0f;
    if (had_player && e.player) e.player.reset();
    if (had_rig && e.vr_rig) e.vr_rig.reset();
    e.name = unique_name(scene_, e.name);
    scene_.entities.push_back(std::move(e));
  }
  scene_.selection = roots;
  push_undo("Paste " + std::to_string(roots.size()), before);
  return roots.size();
}

const std::string& Editor3DScreen::clipboard_text() {
  if (ImGui::GetCurrentContext() != nullptr) {
    if (const char* sys = ImGui::GetClipboardText()) {
      const std::string t(sys);
      if (scene3d::is_3d_scene_text(t)) clipboard_store() = t;
    }
  }
  return clipboard_store();
}

bool Editor3DScreen::copy_selected() {
  const std::string text = copy_json();
  if (text.empty()) return false;
  clipboard_store() = text;
  if (ImGui::GetCurrentContext() != nullptr) ImGui::SetClipboardText(text.c_str());
  note("Copied " + std::to_string(selection_roots().size()) + " to the clipboard");
  return true;
}

std::size_t Editor3DScreen::cut_selected() {
  if (playing() || !copy_selected()) return 0;
  return delete_selected();
}

std::size_t Editor3DScreen::paste() { return paste_json(clipboard_text(), true); }

// ---- Parenting ----------------------------------------------------------------------

bool Editor3DScreen::set_parent(std::uint64_t id, std::uint64_t parent) {
  Entity3D* e = scene_.find(id);
  if (playing() || !e || e->parent == parent || id == parent) return false;
  if (parent != 0) {
    if (!scene_.find(parent)) return false;
    if (contains(scene3d::descendants_of(scene_.entities, id), parent)) return false;  // loop
  }
  const scene3d::Scene3D before = scene_;
  e->parent = parent;
  const Entity3D* p = scene_.find(parent);
  push_undo(p ? "Parent " + e->name + " to " + p->name : "Unparent " + e->name, before);
  return true;
}

bool Editor3DScreen::hierarchy_drop(std::uint64_t id, std::uint64_t target, int where) {
  if (playing() || id == target || !scene_.find(id) || !scene_.find(target)) return false;
  const std::vector<std::uint64_t> subtree = scene3d::descendants_of(scene_.entities, id);
  if (contains(subtree, target)) return false;
  const scene3d::Scene3D before = scene_;
  const std::uint64_t new_parent = where == 2 ? target : scene_.find(target)->parent;
  std::vector<std::uint64_t> block{id};
  block.insert(block.end(), subtree.begin(), subtree.end());
  std::vector<Entity3D> lifted, rest;
  for (const Entity3D& e : scene_.entities) (contains(block, e.id) ? lifted : rest).push_back(e);
  for (Entity3D& e : lifted) {
    if (e.id == id) e.parent = new_parent;
  }
  auto index_of = [&](std::uint64_t want) {
    for (std::size_t i = 0; i < rest.size(); ++i) {
      if (rest[i].id == want) return i;
    }
    return rest.size();
  };
  std::size_t at = index_of(target);
  if (where != 0) {
    std::size_t last = at;
    for (std::uint64_t d : scene3d::descendants_of(rest, target)) last = std::max(last, index_of(d));
    at = std::min(rest.size(), last + 1);
  }
  rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(at), lifted.begin(), lifted.end());
  if (rest == scene_.entities) return false;
  scene_.entities = std::move(rest);
  push_undo(where == 2 ? "Parent " + scene_.find(id)->name : "Reorder " + scene_.find(id)->name,
            before);
  return true;
}

std::uint64_t Editor3DScreen::group_selected() {
  const std::vector<std::uint64_t> roots = selection_roots();
  if (playing() || roots.empty()) return 0;
  const scene3d::Scene3D before = scene_;
  render3d::Aabb box{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
  std::uint64_t common = scene_.find(roots.front())->parent;
  for (std::uint64_t r : roots) {
    const Entity3D* e = scene_.find(r);
    const render3d::Aabb b = world_bounds(*e);
    box.min = render3d::vmin(box.min, b.min);
    box.max = render3d::vmax(box.max, b.max);
    if (e->parent != common) common = 0;
  }
  Entity3D g;
  g.id = scene_.next_id();
  g.name = unique_name(scene_, "Group");
  g.transform.position = {box.center().x, box.min.y, box.center().z};
  g.parent = common;
  // Insert before the first grouped root so the list stays in order.
  std::size_t at = scene_.entities.size();
  for (std::size_t i = 0; i < scene_.entities.size(); ++i) {
    if (contains(roots, scene_.entities[i].id)) {
      at = i;
      break;
    }
  }
  scene_.entities.insert(scene_.entities.begin() + static_cast<std::ptrdiff_t>(at), g);
  for (std::uint64_t r : roots) scene_.find(r)->parent = g.id;
  scene_.selection.assign(1, g.id);
  push_undo("Group " + std::to_string(roots.size()), before);
  return g.id;
}

std::size_t Editor3DScreen::ungroup_selected() {
  if (playing()) return 0;
  const scene3d::Scene3D before = scene_;
  std::size_t freed = 0;
  std::vector<std::uint64_t> drop;
  std::vector<std::uint64_t> new_sel;
  for (std::uint64_t id : selection_roots()) {
    const Entity3D* g = scene_.find(id);
    if (!g) continue;
    const std::uint64_t up = g->parent;
    const std::vector<std::uint64_t> kids = children_of(id);
    for (std::uint64_t k : kids) {
      scene_.find(k)->parent = up;
      new_sel.push_back(k);
      ++freed;
    }
    // A bare empty that only held the group goes away.
    if (!kids.empty() && !g->mesh && !g->light && !g->collider && !g->player &&
        !g->script && !g->vr_rig && !g->grabbable) {
      drop.push_back(id);
    }
  }
  if (freed == 0) return 0;
  scene_.entities.erase(std::remove_if(scene_.entities.begin(), scene_.entities.end(),
                                       [&](const Entity3D& e) { return contains(drop, e.id); }),
                        scene_.entities.end());
  scene_.selection = new_sel;
  push_undo("Ungroup", before);
  return freed;
}

// ---- Lock / hide ----------------------------------------------------------------------

bool Editor3DScreen::set_locked(std::uint64_t id, bool on) {
  const Entity3D* e = scene_.find(id);
  if (!e || playing() || e->locked == on) return false;
  Entity3D c = *e;
  c.locked = on;
  return replace_entity(c, (on ? "Lock " : "Unlock ") + c.name);
}

bool Editor3DScreen::set_hidden(std::uint64_t id, bool on) {
  const Entity3D* e = scene_.find(id);
  if (!e || playing() || e->editor_hidden == on) return false;
  Entity3D c = *e;
  c.editor_hidden = on;
  return replace_entity(c, (on ? "Hide " : "Show ") + c.name);
}

bool Editor3DScreen::lock_selected(bool on) {
  if (playing() || scene_.selection.empty()) return false;
  const scene3d::Scene3D before = scene_;
  for (std::uint64_t id : scene_.selection) {
    if (Entity3D* e = scene_.find(id)) e->locked = on;
  }
  if (scene_ == before) return false;
  push_undo(on ? "Lock" : "Unlock", before);
  return true;
}

bool Editor3DScreen::hide_selected(bool on) {
  if (playing() || scene_.selection.empty()) return false;
  const scene3d::Scene3D before = scene_;
  for (std::uint64_t id : scene_.selection) {
    if (Entity3D* e = scene_.find(id)) e->editor_hidden = on;
  }
  if (scene_ == before) return false;
  push_undo(on ? "Hide" : "Show", before);
  return true;
}

// ---- Align / distribute / ground ------------------------------------------------------

namespace {
float& axis_ref(Vec3& v, int axis) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); }
float axis_of(const Vec3& v, int axis) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); }
}  // namespace

bool Editor3DScreen::align_selected(int axis, int where) {
  const std::vector<std::uint64_t> roots = selection_roots();
  if (playing() || roots.size() < 2 || axis < 0 || axis > 2) return false;
  const scene3d::Scene3D before = scene_;
  float lo = 1e30f, hi = -1e30f;
  std::vector<render3d::Aabb> boxes;
  for (std::uint64_t r : roots) {
    boxes.push_back(world_bounds(*scene_.find(r)));
    lo = std::min(lo, axis_of(boxes.back().min, axis));
    hi = std::max(hi, axis_of(boxes.back().max, axis));
  }
  const float mid = (lo + hi) * 0.5f;
  for (std::size_t i = 0; i < roots.size(); ++i) {
    Entity3D* e = scene_.find(roots[i]);
    const render3d::Transform prev = e->transform;
    float delta = 0.0f;
    if (where < 0) delta = lo - axis_of(boxes[i].min, axis);
    else if (where > 0) delta = hi - axis_of(boxes[i].max, axis);
    else delta = mid - axis_of(boxes[i].center(), axis);
    axis_ref(e->transform.position, axis) += delta;
    carry_descendants(roots[i], prev);
  }
  if (scene_ == before) return false;
  static const char* kAxis[3] = {"X", "Y", "Z"};
  push_undo(std::string("Align ") + kAxis[axis], before);
  return true;
}

bool Editor3DScreen::distribute_selected(int axis) {
  std::vector<std::uint64_t> roots = selection_roots();
  if (playing() || roots.size() < 3 || axis < 0 || axis > 2) return false;
  const scene3d::Scene3D before = scene_;
  std::vector<std::pair<float, std::uint64_t>> by;
  for (std::uint64_t r : roots) by.emplace_back(axis_of(world_bounds(*scene_.find(r)).center(), axis), r);
  std::sort(by.begin(), by.end());
  const float a = by.front().first, b = by.back().first;
  for (std::size_t i = 1; i + 1 < by.size(); ++i) {
    const float want = a + (b - a) * static_cast<float>(i) / static_cast<float>(by.size() - 1);
    Entity3D* e = scene_.find(by[i].second);
    const render3d::Transform prev = e->transform;
    axis_ref(e->transform.position, axis) += want - by[i].first;
    carry_descendants(e->id, prev);
  }
  if (scene_ == before) return false;
  push_undo("Distribute", before);
  return true;
}

std::size_t Editor3DScreen::drop_to_ground() {
  const std::vector<std::uint64_t> roots = selection_roots();
  if (playing() || roots.empty()) return 0;
  const scene3d::Scene3D before = scene_;
  std::size_t moved = 0;
  for (std::uint64_t r : roots) {
    std::vector<std::uint64_t> skip{r};
    for (std::uint64_t d : scene3d::descendants_of(scene_.entities, r)) skip.push_back(d);
    const render3d::Aabb b = world_bounds(*scene_.find(r));
    const Vec3 c = b.center();
    // Highest surface below the bottom centre (ground plane at y = 0).
    float surface = b.min.y >= 0.0f ? 0.0f : b.min.y;
    const render3d::Ray down{{c.x, b.min.y + 1e-3f, c.z}, {0.0f, -1.0f, 0.0f}};
    for (const Entity3D& o : scene_.entities) {
      if (contains(skip, o.id) || !o.visible || effectively_hidden(o) || !o.mesh) continue;
      const render3d::Aabb local =
          view_ ? view_->local_bounds(o) : render3d::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
      float t = 0.0f;
      if (render3d::ray_obb(down, local, o.transform.matrix(), &t) && t >= 0.0f) {
        surface = std::max(surface, down.origin.y - t);
      }
    }
    const float dy = surface - b.min.y;
    if (std::fabs(dy) < 1e-5f) continue;
    Entity3D* e = scene_.find(r);
    const render3d::Transform prev = e->transform;
    e->transform.position.y += dy;
    carry_descendants(r, prev);
    ++moved;
  }
  if (moved == 0) return 0;
  push_undo("Drop to ground", before);
  return moved;
}

bool Editor3DScreen::surface_point(float px, float py, float vw, float vh,
                                   const std::vector<std::uint64_t>& skip, Vec3* out) {
  const render3d::Ray ray = editor_camera().screen_ray(px, py, vw, vh);
  float best = 1e30f;
  float t = 0.0f;
  if (render3d::ray_plane(ray, {0, 1, 0}, 0.0f, &t) && t > 0.0f && t < 1000.0f) best = t;
  std::uint64_t tid = 0;
  if (terrain_ray(ray, &t, &tid) && t < best) best = t;
  for (const Entity3D& o : scene_.entities) {
    if (contains(skip, o.id) || !o.visible || effectively_hidden(o) || !o.mesh) continue;
    const render3d::Aabb local =
        view_ ? view_->local_bounds(o) : render3d::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
    if (render3d::ray_obb(ray, local, o.transform.matrix(), &t) && t > 0.0f && t < best) best = t;
  }
  if (best >= 1e29f) return false;
  *out = ray.origin + ray.dir * best;
  return true;
}

// Alt+drag copies: gizmo_begin_duplicate in Editor3DScreen_Transform.cpp.

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
