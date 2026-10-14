// Editor tools on the 2D workspace: hierarchy (parent links, reorder,
// group), lock / hide, object snapping, resize / rotate handles, align /
// distribute, clipboard and framing. No UI; the viewport, Hierarchy and
// --smoke all drive these.
#include "editor/workspace/Workspace2D.h"

#include "scene/SceneJson.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kMinExtent = 1.0f;

bool contains(const std::vector<std::uint64_t>& v, std::uint64_t id) {
  return std::find(v.begin(), v.end(), id) != v.end();
}

// Rotate (dx, dy) clockwise on screen (y down) by deg.
void turn(float deg, float* dx, float* dy) {
  const float a = deg * kPi / 180.0f;
  const float c = std::cos(a), s = std::sin(a);
  const float x = *dx, y = *dy;
  *dx = x * c - y * s;
  *dy = x * s + y * c;
}

}  // namespace

// --- Hierarchy ---------------------------------------------------------------

std::vector<std::uint64_t> Workspace2D::children_of(std::uint64_t id) const {
  std::vector<std::uint64_t> out;
  for (const Entity2D& e : entities_) {
    if (e.parent == id && e.id != id) out.push_back(e.id);
  }
  return out;
}

std::vector<std::uint64_t> Workspace2D::descendants_of(std::uint64_t id) const {
  std::vector<std::uint64_t> out;
  std::vector<std::uint64_t> stack = children_of(id);
  std::reverse(stack.begin(), stack.end());
  while (!stack.empty() && out.size() <= entities_.size()) {
    const std::uint64_t at = stack.back();
    stack.pop_back();
    if (at == id || contains(out, at)) continue;
    out.push_back(at);
    std::vector<std::uint64_t> kids = children_of(at);
    stack.insert(stack.end(), kids.rbegin(), kids.rend());
  }
  return out;
}

bool Workspace2D::is_ancestor(std::uint64_t ancestor, std::uint64_t id) const {
  const Entity2D* e = find(id);
  std::size_t guard = 0;
  while (e && e->parent != 0 && guard++ <= entities_.size()) {
    if (e->parent == ancestor) return true;
    e = find(e->parent);
  }
  return false;
}

int Workspace2D::depth_of(std::uint64_t id) const {
  int d = 0;
  const Entity2D* e = find(id);
  while (e && e->parent != 0 && d <= static_cast<int>(entities_.size())) {
    e = find(e->parent);
    ++d;
  }
  return d;
}

std::vector<std::uint64_t> Workspace2D::selection_roots() const {
  std::vector<std::uint64_t> out;
  for (std::uint64_t id : selection_) {
    bool covered = false;
    for (std::uint64_t other : selection_) {
      if (other != id && is_ancestor(other, id)) {
        covered = true;
        break;
      }
    }
    if (!covered && find(id)) out.push_back(id);
  }
  return out;
}

bool Workspace2D::effectively_hidden(const Entity2D& e) const {
  const Entity2D* at = &e;
  std::size_t guard = 0;
  while (at && guard++ <= entities_.size()) {
    if (at->hidden) return true;
    at = at->parent ? find(at->parent) : nullptr;
  }
  return false;
}

bool Workspace2D::effectively_locked(const Entity2D& e) const {
  const Entity2D* at = &e;
  std::size_t guard = 0;
  while (at && guard++ <= entities_.size()) {
    if (at->locked) return true;
    at = at->parent ? find(at->parent) : nullptr;
  }
  return false;
}

std::vector<std::uint64_t> Workspace2D::moving_set() const {
  std::vector<std::uint64_t> out;
  for (std::uint64_t root : selection_roots()) {
    const Entity2D* e = find(root);
    if (!e || effectively_locked(*e)) continue;
    if (!contains(out, root)) out.push_back(root);
    for (std::uint64_t d : descendants_of(root)) {
      if (!contains(out, d)) out.push_back(d);
    }
  }
  return out;
}

void Workspace2D::move_subtree(std::uint64_t id, float dx, float dy) {
  if (Entity2D* e = find(id)) {
    e->x += dx;
    e->y += dy;
  }
  for (std::uint64_t d : descendants_of(id)) {
    if (Entity2D* e = find(d)) {
      e->x += dx;
      e->y += dy;
    }
  }
}

bool Workspace2D::set_parent(std::uint64_t id, std::uint64_t parent) {
  Entity2D* e = find(id);
  if (!e || parent == id || (parent != 0 && !find(parent))) return false;
  if (parent != 0 && is_ancestor(id, parent)) return false;  // loop
  if (e->parent == parent) return false;
  e->parent = parent;
  return true;
}

bool Workspace2D::hierarchy_drop(std::uint64_t id, std::uint64_t target,
                                 HierDrop where) {
  if (id == target || !find(id) || !find(target) || is_ancestor(id, target)) {
    return false;
  }
  const std::vector<Entity2D> before = entities_;
  const std::uint64_t new_parent =
      where == HierDrop::Inside ? target : find(target)->parent;
  // Lift the subtree (in its current order) out of the list.
  std::vector<std::uint64_t> block{id};
  for (std::uint64_t d : descendants_of(id)) block.push_back(d);
  std::vector<Entity2D> lifted;
  std::vector<Entity2D> rest;
  for (const Entity2D& e : entities_) {
    (contains(block, e.id) ? lifted : rest).push_back(e);
  }
  // Order the lifted block parent-first as listed in `block`.
  std::vector<Entity2D> ordered;
  for (std::uint64_t b : block) {
    for (const Entity2D& e : lifted) {
      if (e.id == b) ordered.push_back(e);
    }
  }
  ordered.front().parent = new_parent;
  // Insertion point in `rest`.
  std::size_t at = rest.size();
  auto index_of = [&](std::uint64_t want) {
    for (std::size_t i = 0; i < rest.size(); ++i) {
      if (rest[i].id == want) return i;
    }
    return rest.size();
  };
  entities_ = rest;  // descendants_of below reads the list without the block
  if (where == HierDrop::Before) {
    at = index_of(target);
  } else {
    // After the target's own subtree (After), or as its last child (Inside).
    std::size_t last = index_of(target);
    for (std::uint64_t d : descendants_of(target)) last = std::max(last, index_of(d));
    at = std::min(rest.size(), last + 1);
  }
  rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(at), ordered.begin(),
              ordered.end());
  entities_ = std::move(rest);
  return !(entities_ == before);
}

std::uint64_t Workspace2D::group_selection(const std::string& name) {
  const std::vector<std::uint64_t> roots = selection_roots();
  if (roots.empty()) return 0;
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  std::uint64_t common = find(roots.front())->parent;
  std::size_t first_index = entities_.size();
  for (std::uint64_t r : roots) {
    const Entity2D* e = find(r);
    x0 = std::min(x0, e->x);
    y0 = std::min(y0, e->y);
    x1 = std::max(x1, e->x + e->w);
    y1 = std::max(y1, e->y + e->h);
    if (e->parent != common) common = 0;
    for (std::size_t i = 0; i < entities_.size(); ++i) {
      if (entities_[i].id == r) first_index = std::min(first_index, i);
    }
  }
  Entity2D g;
  g.id = alloc_id();
  g.name = unique_name(entities_, name.empty() ? "Group" : name);
  g.x = x0;
  g.y = y0;
  g.w = std::max(kMinExtent, x1 - x0);
  g.h = std::max(kMinExtent, y1 - y0);
  g.color[3] = 0.0f;  // an empty frame: draws nothing at runtime
  g.parent = common;
  int layer = std::numeric_limits<int>::max();
  for (std::uint64_t r : roots) layer = std::min(layer, find(r)->layer);
  g.layer = layer;
  entities_.insert(entities_.begin() + static_cast<std::ptrdiff_t>(first_index), g);
  for (std::uint64_t r : roots) find(r)->parent = g.id;
  select(g.id);
  return g.id;
}

std::size_t Workspace2D::ungroup_selection() {
  std::size_t freed = 0;
  std::vector<std::uint64_t> released;
  std::vector<std::uint64_t> emptied;
  for (std::uint64_t id : selection_roots()) {
    const Entity2D* g = find(id);
    if (!g) continue;
    const std::vector<std::uint64_t> kids = children_of(id);
    if (kids.empty()) continue;
    for (std::uint64_t k : kids) {
      find(k)->parent = g->parent;
      released.push_back(k);
      ++freed;
    }
    emptied.push_back(id);
  }
  // Remove the emptied parents (no children left to orphan).
  entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
                                 [&](const Entity2D& e) { return contains(emptied, e.id); }),
                  entities_.end());
  if (freed > 0) set_selection(released);
  return freed;
}

bool Workspace2D::set_locked(std::uint64_t id, bool locked) {
  Entity2D* e = find(id);
  if (!e || e->locked == locked) return false;
  e->locked = locked;
  return true;
}

bool Workspace2D::set_hidden(std::uint64_t id, bool hidden) {
  Entity2D* e = find(id);
  if (!e || e->hidden == hidden) return false;
  e->hidden = hidden;
  return true;
}

bool Workspace2D::set_selection_locked(bool locked) {
  bool changed = false;
  for (std::uint64_t id : selection_) changed |= set_locked(id, locked);
  return changed;
}

bool Workspace2D::set_selection_hidden(bool hidden) {
  bool changed = false;
  for (std::uint64_t id : selection_) changed |= set_hidden(id, hidden);
  return changed;
}

// --- Object snapping ---------------------------------------------------------

void Workspace2D::snap_box_to_objects(float x0, float y0, float x1, float y1,
                                      const std::vector<std::uint64_t>& moving,
                                      float* dx, float* dy,
                                      std::vector<SnapGuide>* guides) const {
  *dx = 0.0f;
  *dy = 0.0f;
  const float mine_x[3] = {x0, (x0 + x1) * 0.5f, x1};
  const float mine_y[3] = {y0, (y0 + y1) * 0.5f, y1};
  float best_x = snap_threshold_ + 1e-4f, best_y = best_x;
  float snap_x = 0.0f, snap_y = 0.0f;
  bool hit_x = false, hit_y = false;
  for (const Entity2D& e : entities_) {
    if (contains(moving, e.id) || effectively_hidden(e) || e.tilemap) continue;
    const float theirs_x[3] = {e.x, e.center_x(), e.x + e.w};
    const float theirs_y[3] = {e.y, e.center_y(), e.y + e.h};
    for (float m : mine_x) {
      for (float t : theirs_x) {
        if (std::abs(t - m) < best_x) {
          best_x = std::abs(t - m);
          snap_x = t - m;
          hit_x = true;
        }
      }
    }
    for (float m : mine_y) {
      for (float t : theirs_y) {
        if (std::abs(t - m) < best_y) {
          best_y = std::abs(t - m);
          snap_y = t - m;
          hit_y = true;
        }
      }
    }
  }
  if (hit_x) *dx = snap_x;
  if (hit_y) *dy = snap_y;
  if (!guides) return;
  // Every line the snapped box now shares with someone.
  const float sx0 = x0 + *dx, sx1 = x1 + *dx, sy0 = y0 + *dy, sy1 = y1 + *dy;
  const float bx[3] = {sx0, (sx0 + sx1) * 0.5f, sx1};
  const float by[3] = {sy0, (sy0 + sy1) * 0.5f, sy1};
  for (const Entity2D& e : entities_) {
    if (contains(moving, e.id) || effectively_hidden(e) || e.tilemap) continue;
    const float tx[3] = {e.x, e.center_x(), e.x + e.w};
    const float ty[3] = {e.y, e.center_y(), e.y + e.h};
    for (float m : bx) {
      for (float t : tx) {
        if (hit_x && std::abs(t - m) < 0.01f) {
          guides->push_back({true, t, std::min(sy0, e.y), std::max(sy1, e.y + e.h)});
        }
      }
    }
    for (float m : by) {
      for (float t : ty) {
        if (hit_y && std::abs(t - m) < 0.01f) {
          guides->push_back({false, t, std::min(sx0, e.x), std::max(sx1, e.x + e.w)});
        }
      }
    }
  }
}

// --- Resize / rotate handles ---------------------------------------------------

bool Workspace2D::begin_transform() {
  xform_start_.clear();
  xform_active_ = false;
  const Entity2D* p = selected();
  if (!p || effectively_locked(*p)) return false;
  xform_start_.push_back(*p);
  for (std::uint64_t d : descendants_of(p->id)) {
    if (const Entity2D* e = find(d)) xform_start_.push_back(*e);
  }
  xform_active_ = true;
  return true;
}

void Workspace2D::resize_to(int hx, int hy, float wx, float wy, bool keep_aspect) {
  if (!xform_active_ || xform_start_.empty()) return;
  const Entity2D& s0 = xform_start_.front();
  if (s0.tilemap) return;  // a TileMap's size is its grid
  Entity2D* p = find(s0.id);
  if (!p) return;
  const float hw = s0.w * 0.5f, hh = s0.h * 0.5f;
  const float cx = s0.center_x(), cy = s0.center_y();
  // Mouse in the entity's own (unturned) frame, relative to its centre.
  float mx = wx - cx, my = wy - cy;
  turn(-s0.rotation, &mx, &my);
  if (snap_enabled_ && s0.rotation == 0.0f) {
    mx = snap_always(cx + mx) - cx;
    my = snap_always(cy + my) - cy;
  }
  // The opposite side / corner stays put.
  const float ax = -static_cast<float>(hx) * hw, ay = -static_cast<float>(hy) * hh;
  float w = s0.w, h = s0.h;
  if (hx != 0) w = std::max(kMinExtent, static_cast<float>(hx) * (mx - ax));
  if (hy != 0) h = std::max(kMinExtent, static_cast<float>(hy) * (my - ay));
  if (keep_aspect && s0.w > 0.0f && s0.h > 0.0f) {
    float k = 1.0f;
    if (hx != 0 && hy != 0) {
      k = std::max(w / s0.w, h / s0.h);
    } else if (hx != 0) {
      k = w / s0.w;
    } else {
      k = h / s0.h;
    }
    w = std::max(kMinExtent, s0.w * k);
    h = std::max(kMinExtent, s0.h * k);
  }
  // New centre in the local frame, then back to world.
  float lcx = hx != 0 ? ax + static_cast<float>(hx) * w * 0.5f : 0.0f;
  float lcy = hy != 0 ? ay + static_cast<float>(hy) * h * 0.5f : 0.0f;
  if (keep_aspect && hx == 0) lcx = 0.0f;
  if (keep_aspect && hy == 0) lcy = 0.0f;
  float wcx = lcx, wcy = lcy;
  turn(s0.rotation, &wcx, &wcy);
  const float ncx = cx + wcx, ncy = cy + wcy;
  p->w = w;
  p->h = h;
  p->x = ncx - w * 0.5f;
  p->y = ncy - h * 0.5f;
  // Children: same stretch in the parent's frame.
  const float sx = w / std::max(kMinExtent, s0.w), sy = h / std::max(kMinExtent, s0.h);
  for (std::size_t i = 1; i < xform_start_.size(); ++i) {
    const Entity2D& c0 = xform_start_[i];
    Entity2D* c = find(c0.id);
    if (!c) continue;
    // Scale about the old centre, then shift to the new one (keeps the
    // fixed side fixed).
    float lx = c0.center_x() - cx, ly = c0.center_y() - cy;
    turn(-s0.rotation, &lx, &ly);
    lx = lx * sx + lcx;
    ly = ly * sy + lcy;
    turn(s0.rotation, &lx, &ly);
    if (!c0.tilemap) {
      c->w = std::max(kMinExtent, c0.w * sx);
      c->h = std::max(kMinExtent, c0.h * sy);
    }
    c->x = cx + lx - c->w * 0.5f;
    c->y = cy + ly - c->h * 0.5f;
  }
}

void Workspace2D::rotate_to(float delta_deg, float step) {
  if (!xform_active_ || xform_start_.empty()) return;
  const Entity2D& s0 = xform_start_.front();
  Entity2D* p = find(s0.id);
  if (!p) return;
  float target = s0.rotation + delta_deg;
  if (step > 0.0f) target = std::round(target / step) * step;
  // Keep it in (-180, 180].
  target = std::fmod(target, 360.0f);
  if (target > 180.0f) target -= 360.0f;
  if (target <= -180.0f) target += 360.0f;
  const float d = target - s0.rotation;
  p->rotation = target;
  const float cx = s0.center_x(), cy = s0.center_y();
  for (std::size_t i = 1; i < xform_start_.size(); ++i) {
    const Entity2D& c0 = xform_start_[i];
    Entity2D* c = find(c0.id);
    if (!c) continue;
    float lx = c0.center_x() - cx, ly = c0.center_y() - cy;
    turn(d, &lx, &ly);
    c->x = cx + lx - c0.w * 0.5f;
    c->y = cy + ly - c0.h * 0.5f;
    float r = std::fmod(c0.rotation + d, 360.0f);
    if (r > 180.0f) r -= 360.0f;
    if (r <= -180.0f) r += 360.0f;
    c->rotation = c0.tilemap ? 0.0f : r;
  }
}

bool Workspace2D::end_transform() {
  bool changed = false;
  for (const Entity2D& s : xform_start_) {
    if (const Entity2D* e = find(s.id)) changed |= !(*e == s);
  }
  xform_start_.clear();
  xform_active_ = false;
  return changed;
}

void Workspace2D::cancel_transform() {
  for (const Entity2D& s : xform_start_) {
    if (Entity2D* e = find(s.id)) *e = s;
  }
  xform_start_.clear();
  xform_active_ = false;
}

// --- Align / distribute --------------------------------------------------------

bool Workspace2D::align_selection(Align how) {
  std::vector<std::uint64_t> roots;
  for (std::uint64_t id : selection_roots()) {
    const Entity2D* e = find(id);
    if (e && !effectively_locked(*e)) roots.push_back(id);
  }
  if (roots.size() < 2) return false;
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (std::uint64_t id : roots) {
    const Entity2D* e = find(id);
    x0 = std::min(x0, e->x);
    y0 = std::min(y0, e->y);
    x1 = std::max(x1, e->x + e->w);
    y1 = std::max(y1, e->y + e->h);
  }
  bool changed = false;
  for (std::uint64_t id : roots) {
    const Entity2D* e = find(id);
    float dx = 0.0f, dy = 0.0f;
    switch (how) {
      case Align::Left: dx = x0 - e->x; break;
      case Align::CenterX: dx = (x0 + x1) * 0.5f - e->center_x(); break;
      case Align::Right: dx = x1 - (e->x + e->w); break;
      case Align::Top: dy = y0 - e->y; break;
      case Align::CenterY: dy = (y0 + y1) * 0.5f - e->center_y(); break;
      case Align::Bottom: dy = y1 - (e->y + e->h); break;
    }
    if (dx != 0.0f || dy != 0.0f) {
      move_subtree(id, dx, dy);
      changed = true;
    }
  }
  return changed;
}

bool Workspace2D::distribute_selection(bool horizontal) {
  std::vector<const Entity2D*> items;
  for (std::uint64_t id : selection_roots()) {
    const Entity2D* e = find(id);
    if (e && !effectively_locked(*e)) items.push_back(e);
  }
  if (items.size() < 3) return false;
  std::sort(items.begin(), items.end(), [&](const Entity2D* a, const Entity2D* b) {
    return horizontal ? a->x < b->x : a->y < b->y;
  });
  float span_lo = horizontal ? items.front()->x : items.front()->y;
  float span_hi = horizontal ? items.back()->x + items.back()->w
                             : items.back()->y + items.back()->h;
  float total = 0.0f;
  for (const Entity2D* e : items) total += horizontal ? e->w : e->h;
  const float gap = (span_hi - span_lo - total) / static_cast<float>(items.size() - 1);
  std::vector<std::pair<std::uint64_t, float>> moves;  // id, delta
  float at = span_lo;
  for (const Entity2D* e : items) {
    const float cur = horizontal ? e->x : e->y;
    moves.emplace_back(e->id, at - cur);
    at += (horizontal ? e->w : e->h) + gap;
  }
  bool changed = false;
  for (const auto& [id, d] : moves) {
    if (std::abs(d) < 1e-4f) continue;
    move_subtree(id, horizontal ? d : 0.0f, horizontal ? 0.0f : d);
    changed = true;
  }
  return changed;
}

// --- Clipboard -----------------------------------------------------------------

std::string Workspace2D::copy_selection_json() const {
  scene_json::SceneDoc doc;
  for (std::uint64_t root : selection_roots()) {
    if (const Entity2D* e = find(root)) {
      doc.entities.push_back(*e);
      doc.entities.back().parent = 0;  // the clip's own roots
    }
    for (std::uint64_t d : descendants_of(root)) {
      if (const Entity2D* e = find(d)) doc.entities.push_back(*e);
    }
  }
  if (doc.entities.empty()) return "";
  return scene_json::write(doc);
}

std::vector<std::uint64_t> Workspace2D::paste_json(const std::string& text, bool offset) {
  std::vector<std::uint64_t> roots;
  scene_json::SceneDoc doc;
  if (text.empty() || !scene_json::parse(text, &doc, nullptr, "clipboard") ||
      doc.entities.empty()) {
    return roots;
  }
  std::vector<std::pair<std::uint64_t, std::uint64_t>> remap;
  for (const Entity2D& e : doc.entities) remap.emplace_back(e.id, alloc_id());
  auto mapped = [&](std::uint64_t old) -> std::uint64_t {
    for (const auto& [a, b] : remap) {
      if (a == old) return b;
    }
    return 0;
  };
  const float off = offset ? (snap_enabled_ ? grid_size_ : 16.0f) : 0.0f;
  const int top = next_layer();
  for (Entity2D e : doc.entities) {
    const std::uint64_t parent = mapped(e.parent);
    e.id = mapped(e.id);
    e.parent = parent;
    e.name = unique_name(entities_, strip_copy_suffix(e.name));
    e.x += off;
    e.y += off;
    if (parent == 0) {
      e.layer = std::max(e.layer, top);
      e.locked = false;
      roots.push_back(e.id);
    }
    // Cameras following someone outside the clip keep their target only
    // when it exists here.
    if (e.camera && e.camera->target != 0) {
      const std::uint64_t t = mapped(e.camera->target);
      e.camera->target = t != 0 ? t : (find(e.camera->target) ? e.camera->target : 0);
    }
    entities_.push_back(std::move(e));
  }
  set_selection(roots, roots.empty() ? std::nullopt : std::optional<std::uint64_t>(roots.front()));
  return roots;
}

// --- Framing -----------------------------------------------------------------

bool Workspace2D::selection_bounds(float* x0, float* y0, float* x1, float* y1) const {
  float a = 1e30f, b = 1e30f, c = -1e30f, d = -1e30f;
  bool any = false;
  auto add = [&](const Entity2D& e) {
    a = std::min(a, e.x);
    b = std::min(b, e.y);
    c = std::max(c, e.x + e.w);
    d = std::max(d, e.y + e.h);
    any = true;
  };
  if (!selection_.empty()) {
    for (std::uint64_t id : selection_) {
      if (const Entity2D* e = find(id)) add(*e);
    }
  } else {
    for (const Entity2D& e : entities_) {
      if (!effectively_hidden(e)) add(e);
    }
  }
  if (!any) return false;
  *x0 = a;
  *y0 = b;
  *x1 = c;
  *y1 = d;
  return true;
}

bool Workspace2D::frame_selection(float viewport_w, float viewport_h) {
  float x0, y0, x1, y1;
  if (!selection_bounds(&x0, &y0, &x1, &y1)) return false;
  const float w = std::max(16.0f, x1 - x0), h = std::max(16.0f, y1 - y0);
  pan_x_ = (x0 + x1) * 0.5f;
  pan_y_ = (y0 + y1) * 0.5f;
  // 80% of the view, within the zoom limits.
  const float z = std::min(viewport_w * 0.8f / w, viewport_h * 0.8f / h);
  set_zoom(z);
  return true;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
