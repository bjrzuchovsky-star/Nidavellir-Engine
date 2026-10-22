// 2D attach tools (docs/transforms.md "Attach"): attach (keep / snap),
// follow constraint, snap to object, copy transform, align to surface.
// Entities store world positions, so attaching only sets `parent`.
#include "editor/workspace/Workspace2D.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace editor {

bool Workspace2D::attach_to(std::uint64_t id, std::uint64_t target, bool snap) {
  Entity2D* e = find(id);
  const Entity2D* t = find(target);
  if (!e || !t || id == target || is_ancestor(id, target)) return false;
  bool changed = false;
  if (snap) {
    const float dx = t->center_x() - e->center_x(), dy = t->center_y() - e->center_y();
    if (dx != 0.0f || dy != 0.0f) {
      move_subtree(id, dx, dy);
      changed = true;
    }
  }
  if (find(id)->parent != target) changed |= set_parent(id, target);
  return changed;
}

bool Workspace2D::set_follow(std::uint64_t id, std::uint64_t target, bool position, bool rotation) {
  Entity2D* e = find(id);
  const Entity2D* t = find(target);
  if (!e || !t || id == target || (!position && !rotation)) return false;
  FollowData f;
  f.target = target;
  f.position = position;
  f.rotation = rotation;
  float ox = e->center_x() - t->center_x(), oy = e->center_y() - t->center_y();
  if (rotation) {
    // Offset in the target's frame (the runtime turns it with the target).
    const float r = -t->rotation * 3.14159265f / 180.0f;
    const float c = std::cos(r), sn = std::sin(r);
    const float rx = ox * c - oy * sn;
    oy = ox * sn + oy * c;
    ox = rx;
  }
  f.offset_x = ox;
  f.offset_y = oy;
  f.rotation_offset = e->rotation - t->rotation;
  if (e->follow && *e->follow == f) return false;
  e->follow = f;
  return true;
}

bool Workspace2D::snap_to(std::uint64_t id, std::uint64_t target) {
  const Entity2D* e = find(id);
  const Entity2D* t = find(target);
  if (!e || !t || id == target) return false;
  const float dx = t->center_x() - e->center_x(), dy = t->center_y() - e->center_y();
  if (dx == 0.0f && dy == 0.0f) return false;
  move_subtree(id, dx, dy);
  return true;
}

bool Workspace2D::copy_transform(std::uint64_t from, std::uint64_t id, bool position, bool rotation,
                                 bool scale) {
  const Entity2D* f = find(from);
  Entity2D* e = find(id);
  if (!e || !f || id == from) return false;
  const Entity2D before = *e;
  if (scale) {
    // Size around the centre.
    const float cx = e->center_x(), cy = e->center_y();
    e->w = f->w;
    e->h = f->h;
    e->x = cx - e->w * 0.5f;
    e->y = cy - e->h * 0.5f;
  }
  if (rotation) e->rotation = f->rotation;
  if (position) {
    const float dx = f->center_x() - e->center_x(), dy = f->center_y() - e->center_y();
    if (dx != 0.0f || dy != 0.0f) move_subtree(id, dx, dy);
  }
  return !(*find(id) == before);
}

bool Workspace2D::align_to_surface(std::uint64_t id) {
  const Entity2D* e = find(id);
  if (!e || e->tilemap) return false;
  const float x0 = e->x, x1 = e->x + e->w;
  // The surface may start inside the object's lower half.
  const float from = e->y + e->h * 0.5f;
  float best = 1e30f;
  const std::vector<std::uint64_t> kids = descendants_of(id);
  for (const Entity2D& o : entities()) {
    if (o.id == id || o.hidden || std::find(kids.begin(), kids.end(), o.id) != kids.end()) continue;
    if (o.tilemap) {
      const TileMapData& m = *o.tilemap;
      const SolidTable solid = tile_solidity().table(m.tileset);
      const float ts = static_cast<float>(m.tile_size);
      const int c0 = std::max(0, static_cast<int>(std::floor((x0 - o.x) / ts)));
      const int c1 = std::min(m.cols - 1, static_cast<int>(std::floor((x1 - 0.01f - o.x) / ts)));
      for (int c = c0; c <= c1; ++c) {
        for (int r = 0; r < m.rows; ++r) {
          const int tile = m.at(c, r);
          if (tile == 0 || !solid(tile)) continue;
          const float top = o.y + r * ts;
          if (top >= from && top < best) best = top;
          if (top >= from) break;  // the first solid tile down this column
        }
      }
      continue;
    }
    if (!o.collider || o.collider->trigger) continue;
    // Collider box: the entity's top-left plus the offset (World_Collision).
    const float l = o.x + o.collider->offset_x, rgt = l + o.collider->w;
    const float top = o.y + o.collider->offset_y;
    if (rgt <= x0 || l >= x1) continue;
    if (top >= from && top < best) best = top;
  }
  if (best >= 1e29f) return false;
  Entity2D* m = find(id);
  const float dy = best - (m->y + m->h);
  const bool turned = m->rotation != 0.0f;
  m->rotation = 0.0f;  // upright on the (flat) top
  if (dy != 0.0f) move_subtree(id, 0.0f, dy);
  return turned || dy != 0.0f;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
