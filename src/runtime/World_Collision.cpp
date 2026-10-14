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
  if (e.collider) {
    const Aabb a = collider_box(e, x, y);
    if (in_view(a, view)) {
      OverlayBox b;
      b.x0 = a.x0;
      b.y0 = a.y0;
      b.x1 = a.x1;
      b.y1 = a.y1;
      b.kind = kind_of(*e.collider);
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
  for (std::size_t idx : static_solids_) {
    if (&actors_[idx] != &a) {
      consider(collider_box(actors_[idx].data));
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
  if (axis == 0) {
    a.data.x += delta > 0.0f ? edge - now.x1 : edge - now.x0;
  } else {
    a.data.y += delta > 0.0f ? edge - now.y1 : edge - now.y0;
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
    for (const Actor& o : actors_) {
      if (&o == &trig) {
        continue;
      }
      // Riders and other moving solid bodies set triggers off.
      const bool mover =
          o.data.player.has_value() ||
          (solid_body(o.data) && o.data.collider->dynamic);
      if (mover && overlaps(tb, collider_box(o.data))) {
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
