// Whole-selection transforms for the 2D modal G / R / S and the 2D gizmo
// (docs/transforms.md). Positions are world space; children follow their
// root (same offset, orbit or stretch), as with the handle drags.

#include "editor/workspace/Workspace2D.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kMinExtent = 1.0f;

// Rotate (dx, dy) clockwise on screen (y down) by deg.
void turn(float deg, float* dx, float* dy) {
  const float a = deg * kPi / 180.0f;
  const float c = std::cos(a), s = std::sin(a);
  const float x = *dx, y = *dy;
  *dx = x * c - y * s;
  *dy = x * s + y * c;
}

float wrap_deg(float r) {
  r = std::fmod(r, 360.0f);
  if (r > 180.0f) r -= 360.0f;
  if (r <= -180.0f) r += 360.0f;
  return r;
}

}  // namespace

bool Workspace2D::begin_xform() {
  modal_roots_.clear();
  modal_subtrees_.clear();
  modal_active_ = false;
  for (std::uint64_t id : selection_roots()) {
    const Entity2D* e = find(id);
    if (!e || effectively_locked(*e)) continue;
    modal_roots_.push_back(*e);
    std::vector<Entity2D> sub;
    for (std::uint64_t d : descendants_of(id)) {
      if (const Entity2D* c = find(d)) sub.push_back(*c);
    }
    modal_subtrees_.push_back(std::move(sub));
  }
  modal_active_ = !modal_roots_.empty();
  return modal_active_;
}

void Workspace2D::apply_xform(const Xform2D& x) {
  if (!modal_active_) return;
  for (std::size_t i = 0; i < modal_roots_.size(); ++i) {
    const Entity2D& r0 = modal_roots_[i];
    const float pivot_x = x.individual ? r0.center_x() : x.pivot_x;
    const float pivot_y = x.individual ? r0.center_y() : x.pivot_y;
    const float frame = x.individual ? r0.rotation : x.frame_deg;
    auto place = [&](const Entity2D& e0) {
      Entity2D* e = find(e0.id);
      if (!e) return;
      *e = e0;
      switch (x.op) {
        case Xform2D::Op::Move:
          e->x = e0.x + x.dx;
          e->y = e0.y + x.dy;
          break;
        case Xform2D::Op::Rotate: {
          float vx = e0.center_x() - pivot_x, vy = e0.center_y() - pivot_y;
          turn(x.degrees, &vx, &vy);
          e->x = pivot_x + vx - e0.w * 0.5f;
          e->y = pivot_y + vy - e0.h * 0.5f;
          e->rotation = e0.tilemap ? 0.0f : wrap_deg(e0.rotation + x.degrees);
          break;
        }
        case Xform2D::Op::Scale: {
          float vx = e0.center_x() - pivot_x, vy = e0.center_y() - pivot_y;
          turn(-frame, &vx, &vy);
          vx *= x.sx;
          vy *= x.sy;
          turn(frame, &vx, &vy);
          if (!e0.tilemap) {  // a TileMap's size is its grid
            e->w = std::max(kMinExtent, e0.w * std::fabs(x.sx));
            e->h = std::max(kMinExtent, e0.h * std::fabs(x.sy));
          }
          e->x = pivot_x + vx - e->w * 0.5f;
          e->y = pivot_y + vy - e->h * 0.5f;
          break;
        }
      }
    };
    place(r0);
    for (const Entity2D& c0 : modal_subtrees_[i]) place(c0);
  }
}

bool Workspace2D::end_xform() {
  bool changed = false;
  for (std::size_t i = 0; i < modal_roots_.size(); ++i) {
    if (const Entity2D* e = find(modal_roots_[i].id)) changed |= !(*e == modal_roots_[i]);
    for (const Entity2D& c0 : modal_subtrees_[i]) {
      if (const Entity2D* e = find(c0.id)) changed |= !(*e == c0);
    }
  }
  modal_roots_.clear();
  modal_subtrees_.clear();
  modal_active_ = false;
  return changed;
}

void Workspace2D::cancel_xform() {
  for (std::size_t i = 0; i < modal_roots_.size(); ++i) {
    if (Entity2D* e = find(modal_roots_[i].id)) *e = modal_roots_[i];
    for (const Entity2D& c0 : modal_subtrees_[i]) {
      if (Entity2D* e = find(c0.id)) *e = c0;
    }
  }
  modal_roots_.clear();
  modal_subtrees_.clear();
  modal_active_ = false;
}

bool Workspace2D::clear_selection_transform(Xform2D::Op what) {
  if (modal_active_) return false;
  const Snapshot before = snapshot();
  if (!begin_xform()) return false;
  const std::vector<Entity2D> roots = modal_roots_;
  const std::vector<std::vector<Entity2D>> subs = modal_subtrees_;
  modal_active_ = false;
  for (std::size_t i = 0; i < roots.size(); ++i) {
    // Per root: the transform that takes it to the cleared value.
    Xform2D x;
    x.op = what;
    const Entity2D& r0 = roots[i];
    if (what == Xform2D::Op::Move) {
      x.dx = -r0.x;
      x.dy = -r0.y;
    } else if (what == Xform2D::Op::Rotate) {
      x.degrees = -r0.rotation;
      x.pivot_x = r0.center_x();
      x.pivot_y = r0.center_y();
    } else {
      if (r0.tilemap) continue;
      x.sx = 64.0f / std::max(kMinExtent, r0.w);
      x.sy = 64.0f / std::max(kMinExtent, r0.h);
      x.individual = true;
    }
    modal_roots_ = {r0};
    modal_subtrees_ = {subs[i]};
    modal_active_ = true;
    apply_xform(x);
    modal_active_ = false;
  }
  modal_roots_.clear();
  modal_subtrees_.clear();
  const char* label = what == Xform2D::Op::Move
                          ? "Clear Location"
                          : (what == Xform2D::Op::Rotate ? "Clear Rotation" : "Clear Scale");
  return commit_step(label, before);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
