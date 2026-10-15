#include "runtime/Collision.h"
#include "runtime/World.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace ts {
namespace tombstone {
namespace runtime {

// --- Shapes ---------------------------------------------------------------------

bool overlaps(const Aabb& a, const Aabb& b) {
  return a.x0 < b.x1 - kContactSlop && a.x1 > b.x0 + kContactSlop &&
         a.y0 < b.y1 - kContactSlop && a.y1 > b.y0 + kContactSlop;
}

Aabb collider_box(const Entity2D& e, float x, float y) {
  if (!e.collider) {
    return Aabb{x, y, x + e.w, y + e.h};
  }
  const ColliderData& c = *e.collider;
  const float x0 = x + c.offset_x;
  const float y0 = y + c.offset_y;
  return Aabb{x0, y0, x0 + c.w, y0 + c.h};
}

bool turned(const Entity2D& e) {
  const float r = std::fmod(e.rotation, 360.0f);
  return std::isfinite(r) && r != 0.0f;
}

Obb collider_obb(const Entity2D& e, float x, float y) {
  const Aabb a = collider_box(e, x, y);
  Obb b;
  b.hx = (a.x1 - a.x0) * 0.5f;
  b.hy = (a.y1 - a.y0) * 0.5f;
  const float px = x + e.w * 0.5f;  // pivot: entity centre
  const float py = y + e.h * 0.5f;
  const float dx = (a.x0 + a.x1) * 0.5f - px;
  const float dy = (a.y0 + a.y1) * 0.5f - py;
  if (!turned(e)) {
    b.cx = px + dx;
    b.cy = py + dy;
    return b;
  }
  const double rad = static_cast<double>(e.rotation) * 3.14159265358979323846 / 180.0;
  b.c = static_cast<float>(std::cos(rad));
  b.s = static_cast<float>(std::sin(rad));
  // y down: (c, -s; s, c) turns clockwise on screen.
  b.cx = px + dx * b.c - dy * b.s;
  b.cy = py + dx * b.s + dy * b.c;
  return b;
}

Aabb bounds(const Obb& b) {
  const float ex = b.hx * std::fabs(b.c) + b.hy * std::fabs(b.s);
  const float ey = b.hx * std::fabs(b.s) + b.hy * std::fabs(b.c);
  return Aabb{b.cx - ex, b.cy - ey, b.cx + ex, b.cy + ey};
}

bool overlap_shifts(const Aabb& a, const Obb& b, int axis, float* lo,
                    float* hi) {
  const float ax = (a.x0 + a.x1) * 0.5f, ay = (a.y0 + a.y1) * 0.5f;
  const float ahx = (a.x1 - a.x0) * 0.5f, ahy = (a.y1 - a.y0) * 0.5f;
  const float axes[4][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {b.c, b.s}, {-b.s, b.c}};
  float l = std::numeric_limits<float>::lowest();
  float h = std::numeric_limits<float>::max();
  for (const auto& n : axes) {
    const float ra = ahx * std::fabs(n[0]) + ahy * std::fabs(n[1]);
    const float rb = b.hx * std::fabs(n[0] * b.c + n[1] * b.s) +
                     b.hy * std::fabs(-n[0] * b.s + n[1] * b.c);
    const float reach = ra + rb - kContactSlop;
    if (reach <= 0.0f) return false;
    const float d = (n[0] * b.cx + n[1] * b.cy) - (n[0] * ax + n[1] * ay);
    const float k = n[axis];  // how a shift along `axis` moves the projection
    if (std::fabs(k) < 1.0e-6f) {
      if (std::fabs(d) >= reach) return false;  // this axis separates for any t
      continue;
    }
    float t0 = (d - reach) / k, t1 = (d + reach) / k;
    if (t0 > t1) std::swap(t0, t1);
    l = std::max(l, t0);
    h = std::min(h, t1);
    if (l >= h) return false;
  }
  *lo = l;
  *hi = h;
  return true;
}

bool overlaps(const Aabb& a, const Obb& b) {
  float lo = 0.0f, hi = 0.0f;
  return overlap_shifts(a, b, 0, &lo, &hi) && lo < 0.0f && hi > 0.0f;
}

std::vector<std::pair<float, float>> overlay_outline(const OverlayBox& b) {
  std::vector<std::pair<float, float>> pts;
  const float cx = (b.x0 + b.x1) * 0.5f, cy = (b.y0 + b.y1) * 0.5f;
  const float hx = (b.x1 - b.x0) * 0.5f, hy = (b.y1 - b.y0) * 0.5f;
  constexpr float kPi = 3.14159265f;
  if (b.shape == ColliderData::Shape::Circle) {
    const float r = std::min(hx, hy);
    for (int i = 0; i < 24; ++i) {
      const float a = 2.0f * kPi * static_cast<float>(i) / 24.0f;
      pts.emplace_back(std::cos(a) * r, std::sin(a) * r);
    }
  } else if (b.shape == ColliderData::Shape::Capsule) {
    // Round ends on the long axis.
    const bool wide = hx >= hy;
    const float r = wide ? hy : hx, half = (wide ? hx : hy) - r;
    for (int end = 0; end < 2; ++end) {
      for (int i = 0; i <= 12; ++i) {
        const float a = kPi * static_cast<float>(i) / 12.0f + (end ? kPi : 0.0f) + (wide ? -kPi * 0.5f : 0.0f);
        const float s = end ? -half : half;
        pts.emplace_back(std::cos(a) * r + (wide ? s : 0.0f), std::sin(a) * r + (wide ? 0.0f : s));
      }
    }
  } else {
    pts = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
  }
  const float rad = b.rotation * kPi / 180.0f;
  const float c = std::cos(rad), s = std::sin(rad);
  for (auto& [x, y] : pts) {
    const float rx = x * c - y * s, ry = x * s + y * c;
    x = cx + rx;
    y = cy + ry;
  }
  return pts;
}

namespace {

bool in_view(const Aabb& b, const WorldRect& view) {
  return b.x1 >= view.x0 && b.x0 <= view.x1 && b.y1 >= view.y0 &&
         b.y0 <= view.y1;
}

OverlayKind kind_of(const ColliderData& c) {
  if (c.trigger) {
    return OverlayKind::Trigger;
  }
  return c.dynamic ? OverlayKind::DynamicSolid : OverlayKind::StaticSolid;
}

bool solid_body(const Entity2D& e) {
  return e.collider && !e.collider->trigger;
}

// Cells of a tilemap whose top-left corner is (tx, ty) that a box touches,
// clamped to the grid. False when the box misses the grid entirely.
bool cell_range(const TileMapData& tm, float tx, float ty, const Aabb& b,
                int* c0, int* r0, int* c1, int* r1) {
  const float ts = static_cast<float>(tm.tile_size);
  *c0 = std::max(0, static_cast<int>(std::floor((b.x0 - tx) / ts)));
  *r0 = std::max(0, static_cast<int>(std::floor((b.y0 - ty) / ts)));
  *c1 = std::min(tm.cols - 1, static_cast<int>(std::floor((b.x1 - tx) / ts)));
  *r1 = std::min(tm.rows - 1, static_cast<int>(std::floor((b.y1 - ty) / ts)));
  return *c0 <= *c1 && *r0 <= *r1;
}

}  // namespace

void append_overlay(const Entity2D& e, float x, float y,
                    const TileSolidity& solidity, const WorldRect& view,
                    std::vector<OverlayBox>* out) {
  if (e.tilemap) {
    const TileMapData& tm = *e.tilemap;
    const SolidTable table = solidity.table(tm.tileset);
    int c0 = 0;
    int r0 = 0;
    int c1 = 0;
    int r1 = 0;
    const Aabb v{view.x0, view.y0, view.x1, view.y1};
    if (table.any() && cell_range(tm, x, y, v, &c0, &r0, &c1, &r1)) {
      const float ts = static_cast<float>(tm.tile_size);
      for (int r = r0; r <= r1; ++r) {
        int run = -1;  // first column of the current solid run
        for (int c = c0; c <= c1 + 1; ++c) {
          const bool solid = c <= c1 && table(tm.at(c, r));
          if (solid && run < 0) {
            run = c;
          } else if (!solid && run >= 0) {
            OverlayBox b;
            b.x0 = x + static_cast<float>(run) * ts;
            b.y0 = y + static_cast<float>(r) * ts;
            b.x1 = x + static_cast<float>(c) * ts;
            b.y1 = b.y0 + ts;
            b.kind = OverlayKind::SolidTile;
            b.entity = e.id;
            out->push_back(b);
            run = -1;
          }
        }
      }
    }
  }
  if (e.collider || e.body) {
    const Aabb a = collider_box(e, x, y);
    const Obb o = collider_obb(e, x, y);
    if (in_view(bounds(o), view)) {
      OverlayBox b;
      b.x0 = a.x0;
      b.y0 = a.y0;
      b.x1 = a.x1;
      b.y1 = a.y1;
      // Turned boxes keep their size and move their centre to the pivot.
      b.x0 = o.cx - o.hx;
      b.x1 = o.cx + o.hx;
      b.y0 = o.cy - o.hy;
      b.y1 = o.cy + o.hy;
      (void)a;
      b.rotation = turned(e) ? e.rotation : 0.0f;
      b.kind = e.collider ? kind_of(*e.collider)
               : e.body->motion == RigidBody2DData::Motion::Dynamic
                   ? OverlayKind::DynamicSolid
                   : OverlayKind::StaticSolid;
      b.body = e.body.has_value();
      if (e.collider) b.shape = e.collider->shape;
      b.entity = e.id;
      out->push_back(b);
    }
  }
}

// --- World ------------------------------------------------------------------------

void World::setup_collision() {
  grids_.clear();
  static_solids_.clear();
  dynamics_.clear();
  triggers_.clear();
  body_blockers_.clear();
  inside_.clear();
  events_.clear();
  last_event_.reset();
  events_total_ = 0;
  // Substep limit: half the thinnest thing a rider could hit, so even a
  // 5000 px/s rider never skips past a wall within one substep.
  float thinnest = std::numeric_limits<float>::max();
  for (std::size_t i = 0; i < actors_.size(); ++i) {
    const Entity2D& e = actors_[i].data;
    if (e.tilemap) {
      SolidGrid g;
      g.actor = i;
      g.table = solidity_.table(e.tilemap->tileset);
      if (g.table.any()) {
        thinnest = std::min(thinnest,
                            static_cast<float>(e.tilemap->tile_size));
        grids_.push_back(std::move(g));
      }
    }
    if (e.body) {
      // Box2D owns it (setup_physics2d); static / kinematic ones still
      // block riders.
      if (e.body->motion != RigidBody2DData::Motion::Dynamic &&
          !(e.collider && e.collider->trigger)) {
        body_blockers_.push_back(i);
        const Aabb a = collider_box(e);
        thinnest = std::min(thinnest, std::min(a.x1 - a.x0, a.y1 - a.y0));
      }
      continue;
    }
    if (!e.collider) {
      continue;
    }
    if (e.collider->trigger) {
      triggers_.push_back(i);
    } else if (e.collider->dynamic) {
      dynamics_.push_back(i);
    } else {
      static_solids_.push_back(i);
      thinnest = std::min(thinnest, std::min(e.collider->w, e.collider->h));
    }
  }
  step_limit_ = thinnest < std::numeric_limits<float>::max()
                    ? std::max(0.25f, thinnest * 0.5f)
                    : 1.0e9f;
  setup_physics2d();
}

void World::move_body(Actor& a, float dx, float dy) {
  if (!solid_body(a.data)) {
    a.data.x += dx;
    a.data.y += dy;
    return;
  }
  const ColliderData& c = *a.data.collider;
  const float limit =
      std::max(0.25f, std::min(step_limit_, 0.5f * std::min(c.w, c.h)));
  const float dist = std::max(std::fabs(dx), std::fabs(dy));
  int n = static_cast<int>(std::ceil(dist / limit));
  n = std::clamp(n, 1, 4096);
  const float sx = dx / static_cast<float>(n);
  const float sy = dy / static_cast<float>(n);
  for (int i = 0; i < n; ++i) {
    if (sx != 0.0f) {
      a.data.x += sx;
      resolve_axis(a, 0, sx);
    }
    if (sy != 0.0f) {
      a.data.y += sy;
      resolve_axis(a, 1, sy);
    }
  }
}

void World::resolve_axis(Actor& a, int axis, float delta) {
  const Aabb now = collider_box(a.data);
  Aabb before = now;
  if (axis == 0) {
    before.x0 -= delta;
    before.x1 -= delta;
  } else {
    before.y0 -= delta;
    before.y1 -= delta;
  }
  bool hit = false;
  float edge = delta > 0.0f ? std::numeric_limits<float>::max()
                            : std::numeric_limits<float>::lowest();
  // Only what this move ran into counts; something the body already
  // overlapped (spawned inside a wall) lets it walk back out.
  auto consider = [&](const Aabb& o) {
    if (!overlaps(now, o) || overlaps(before, o)) {
      return;
    }
    hit = true;
    if (axis == 0) {
      edge = delta > 0.0f ? std::min(edge, o.x0) : std::max(edge, o.x1);
    } else {
      edge = delta > 0.0f ? std::min(edge, o.y0) : std::max(edge, o.y1);
    }
  };
  for (const SolidGrid& g : grids_) {
    const Actor& t = actors_[g.actor];
    if (&t == &a) {
      continue;
    }
    const TileMapData& tm = *t.data.tilemap;
    int c0 = 0;
    int r0 = 0;
    int c1 = 0;
    int r1 = 0;
    if (!cell_range(tm, t.data.x, t.data.y, now, &c0, &r0, &c1, &r1)) {
      continue;
    }
    const float ts = static_cast<float>(tm.tile_size);
    for (int r = r0; r <= r1; ++r) {
      for (int c = c0; c <= c1; ++c) {
        if (!g.table(tm.at(c, r))) {
          continue;
        }
        const float x0 = t.data.x + static_cast<float>(c) * ts;
        const float y0 = t.data.y + static_cast<float>(r) * ts;
        consider(Aabb{x0, y0, x0 + ts, y0 + ts});
      }
    }
  }
  // Turned boxes: the shift that backs out of each one (the SAT interval
  // of shifts along this axis that still overlap).
  float shift = 0.0f;
  auto consider_obb = [&](const Obb& o) {
    float lo = 0.0f, hi = 0.0f;
    if (!overlap_shifts(now, o, axis, &lo, &hi) || !(lo < 0.0f && hi > 0.0f)) {
      return;  // not overlapping now
    }
    // Already inside before this move (by more than float noise: a rider
    // left flush last substep sits right on the interval's edge).
    if (lo + kContactSlop < -delta && -delta < hi - kContactSlop) {
      return;
    }
    hit = true;
    shift = delta > 0.0f ? std::min(shift, lo) : std::max(shift, hi);
  };
  auto consider_entity = [&](const Entity2D& e) {
    if (turned(e)) {
      consider_obb(collider_obb(e));
    } else {
      consider(collider_box(e));
    }
  };
  for (std::size_t idx : static_solids_) {
    if (&actors_[idx] != &a) {
      consider_entity(actors_[idx].data);
    }
  }
  for (std::size_t idx : body_blockers_) {
    if (&actors_[idx] != &a) {
      consider_entity(actors_[idx].data);
    }
  }
  if (ext_ && !ext_->blockers.empty()) {
    // Plugin solids (a chunked survival map...) around the swept box.
    Aabb area{std::min(now.x0, before.x0), std::min(now.y0, before.y0),
              std::max(now.x1, before.x1), std::max(now.y1, before.y1)};
    for (const WorldExtensions::Blocker& b : ext_->blockers) {
      if (!b.solids) continue;
      blocker_boxes_.clear();
      b.solids(*this, area, &blocker_boxes_);
      for (const Aabb& o : blocker_boxes_) consider(o);
    }
  }
  if (!hit) {
    return;
  }
  // Back out along this axis only: flush against the nearest face.
  float back = shift;
  const bool box_hit = delta > 0.0f ? edge < std::numeric_limits<float>::max()
                                    : edge > std::numeric_limits<float>::lowest();
  if (box_hit) {
    const float b = axis == 0 ? (delta > 0.0f ? edge - now.x1 : edge - now.x0)
                              : (delta > 0.0f ? edge - now.y1 : edge - now.y0);
    back = delta > 0.0f ? std::min(back, b) : std::max(back, b);
  }
  if (axis == 0) {
    a.data.x += back;
  } else {
    a.data.y += back;
  }
}

void World::separate_dynamics() {
  // Simple separation: push each overlapping pair apart along the axis of
  // least overlap, half each. A body pinned on a wall moves less and its
  // partner takes the rest. A few passes settle a small crowd.
  for (int pass = 0; pass < 4; ++pass) {
    bool any = false;
    for (std::size_t i = 0; i < dynamics_.size(); ++i) {
      for (std::size_t j = i + 1; j < dynamics_.size(); ++j) {
        Actor& a = actors_[dynamics_[i]];
        Actor& b = actors_[dynamics_[j]];
        const Aabb ba = collider_box(a.data);
        const Aabb bb = collider_box(b.data);
        const float ox = std::min(ba.x1, bb.x1) - std::max(ba.x0, bb.x0);
        const float oy = std::min(ba.y1, bb.y1) - std::max(ba.y0, bb.y0);
        if (ox <= kContactSlop || oy <= kContactSlop) {
          continue;
        }
        any = true;
        const int axis = ox <= oy ? 0 : 1;
        const float depth = axis == 0 ? ox : oy;
        const float ca = axis == 0 ? ba.x0 + ba.x1 : ba.y0 + ba.y1;
        const float cb = axis == 0 ? bb.x0 + bb.x1 : bb.y0 + bb.y1;
        // a goes toward its own side (the earlier one wins a dead tie).
        const float dir = ca <= cb ? -1.0f : 1.0f;
        float& pa = axis == 0 ? a.data.x : a.data.y;
        const float a_before = pa;
        const float half = depth * 0.5f;
        if (axis == 0) {
          move_body(a, dir * half, 0.0f);
        } else {
          move_body(a, 0.0f, dir * half);
        }
        const float rest = depth - std::fabs(pa - a_before);
        if (axis == 0) {
          move_body(b, -dir * rest, 0.0f);
        } else {
          move_body(b, 0.0f, -dir * rest);
        }
      }
    }
    if (!any) {
      break;
    }
  }
}

void World::update_triggers() {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> now;
  for (std::size_t t : triggers_) {
    const Actor& trig = actors_[t];
    const Aabb tb = collider_box(trig.data);
    const bool tturn = turned(trig.data);
    const Obb tob = collider_obb(trig.data);
    for (const Actor& o : actors_) {
      if (&o == &trig) {
        continue;
      }
      // Riders and other moving solid bodies set triggers off.
      // Riders, moving solid bodies and dynamic rigid bodies set triggers off.
      const bool mover =
          o.data.player.has_value() ||
          (o.data.body ? o.data.body->motion != RigidBody2DData::Motion::Static
                       : solid_body(o.data) && o.data.collider->dynamic);
      if (!mover || (o.data.collider && o.data.collider->trigger)) continue;
      const Aabb ob = o.data.body && turned(o.data) ? bounds(collider_obb(o.data))
                                                    : collider_box(o.data);
      if (tturn ? overlaps(ob, tob) : overlaps(tb, ob)) {
        now.emplace_back(trig.data.id, o.data.id);
      }
    }
  }
  std::sort(now.begin(), now.end());
  now.erase(std::unique(now.begin(), now.end()), now.end());
  std::vector<std::pair<std::uint64_t, std::uint64_t>> left;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> entered;
  std::set_difference(inside_.begin(), inside_.end(), now.begin(), now.end(),
                      std::back_inserter(left));
  std::set_difference(now.begin(), now.end(), inside_.begin(), inside_.end(),
                      std::back_inserter(entered));
  const std::uint64_t this_tick = tick_ + 1;
  for (const auto& [trig, other] : left) {
    events_.push_back({TriggerEvent::Kind::Exit, trig, other, this_tick});
  }
  for (const auto& [trig, other] : entered) {
    events_.push_back({TriggerEvent::Kind::Enter, trig, other, this_tick});
  }
  if (!events_.empty()) {
    last_event_ = events_.back();
    events_total_ += events_.size();
  }
  inside_ = std::move(now);
}

bool World::inside_trigger(std::uint64_t trigger, std::uint64_t other) const {
  return std::binary_search(inside_.begin(), inside_.end(),
                            std::make_pair(trigger, other));
}

std::string World::describe(const TriggerEvent& event) const {
  auto name_of = [&](std::uint64_t id) {
    const Actor* a = find(id);
    return a ? a->data.name : "#" + std::to_string(id);
  };
  return name_of(event.other) +
         (event.kind == TriggerEvent::Kind::Enter ? " rode into "
                                                  : " rode out of ") +
         name_of(event.trigger);
}

void World::build_overlay(const WorldRect& view, float alpha,
                          std::vector<OverlayBox>* out) const {
  out->clear();
  alpha = std::clamp(alpha, 0.0f, 1.0f);
  for (std::size_t idx : draw_order_) {
    const Actor& a = actors_[idx];
    const float x = a.prev_x + (a.data.x - a.prev_x) * alpha;
    const float y = a.prev_y + (a.data.y - a.prev_y) * alpha;
    const std::size_t first = out->size();
    append_overlay(a.data, x, y, solidity_, view, out);
    for (std::size_t k = first; k < out->size(); ++k) {
      OverlayBox& b = (*out)[k];
      if (b.kind != OverlayKind::Trigger) {
        continue;
      }
      const auto lo = std::lower_bound(
          inside_.begin(), inside_.end(),
          std::make_pair(a.data.id, std::uint64_t{0}));
      b.active = lo != inside_.end() && lo->first == a.data.id;
    }
  }
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
