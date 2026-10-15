// Networking hooks (see World.h): server-side rider assignment and snapshot
// capture, client-side snapshot apply, rider prediction and animation.

#include "runtime/World.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace ts {
namespace tombstone {
namespace runtime {

std::uint64_t World::ensure_rider(int slot, std::uint64_t forced_id) {
  if (slot < 0 || slot >= kMaxPlayers) {
    return 0;
  }
  if (const Actor* a = player(slot)) {
    return a->data.id;
  }
  if (forced_id != 0 && find(forced_id)) {
    return forced_id;
  }
  // Template: slot 0's rider, else the first rider in the scene.
  const Actor* tmpl = player(0);
  for (const Actor& a : actors_) {
    if (!tmpl && a.data.player && !a.dead) tmpl = &a;
  }
  if (!tmpl || actors_.size() >= kMaxActors) {
    return 0;
  }
  Actor a;
  a.data = tmpl->data;
  if (!a.data.collider && tmpl->stashed) {
    a.data.collider = tmpl->stashed;
  }
  a.data.player->slot = slot;
  a.data.camera.reset();
  a.data.spawn.reset();
  a.data.name = tmpl->data.name + " " + std::to_string(slot + 1);
  // Start where the build would have put it: on the slot's SpawnPoint,
  // else a little to the side of the template rider.
  bool placed = false;
  for (const Actor& s : actors_) {
    if (s.data.spawn && s.data.spawn->slot == slot) {
      a.data.x = s.data.center_x() - a.data.w * 0.5f;
      a.data.y = s.data.center_y() - a.data.h * 0.5f;
      placed = true;
      break;
    }
  }
  if (!placed) {
    const Actor* t0 = player(0) ? player(0) : tmpl;
    a.data.x = t0->data.x + static_cast<float>(slot) * (a.data.w + 8.0f);
    a.data.y = t0->data.y;
  }
  a.data.id = forced_id != 0 ? forced_id : next_id_;
  next_id_ = std::max(next_id_, a.data.id + 1);
  a.prev_x = a.data.x;
  a.prev_y = a.data.y;
  setup_actor_animation(a);
  const std::uint64_t id = a.data.id;
  const bool scripted = a.data.script.has_value();
  actors_.push_back(std::move(a));
  dirty_ = true;
  if (scripts_ && scripted) {
    const Entity2D copy = actors_.back().data;
    scripts_->attach(copy);
  }
  apply_changes();
  return id;
}

void World::predict_rider(std::uint64_t id, const PlayerInput& in) {
  Actor* a = live(id);
  if (!a || !a->data.player) {
    return;
  }
  apply_changes();
  a = live(id);
  a->prev_x = a->data.x;
  a->prev_y = a->data.y;
  move_rider(*a, in);
}

void World::net_capture(std::vector<NetActor>* out) const {
  out->clear();
  out->reserve(actors_.size());
  for (const Actor& a : actors_) {
    if (a.dead) continue;
    NetActor r;
    r.id = a.data.id;
    r.x = a.data.x;
    r.y = a.data.y;
    r.rotation = a.data.rotation;
    r.in_x = a.in_x;
    r.in_y = a.in_y;
    r.facing = static_cast<std::int8_t>(a.facing < 0 ? -1 : 1);
    r.dir = a.dir;
    r.hidden = a.hidden;
    r.collider_on = a.data.collider.has_value();
    r.slot = a.data.player ? a.data.player->slot : -1;
    r.spawned = a.spawned;
    if (a.spawned) r.name = a.data.name;
    out->push_back(std::move(r));
  }
}

void World::net_apply(const std::vector<NetActor>& rows) {
  std::unordered_set<std::uint64_t> seen;
  seen.reserve(rows.size());
  for (const NetActor& r : rows) {
    seen.insert(r.id);
    Actor* a = live(r.id);
    if (!a) {
      std::uint64_t made = 0;
      if (r.slot >= 0 && r.slot < kMaxPlayers && !player(r.slot)) {
        made = ensure_rider(r.slot, r.id);
      } else if (r.spawned && actors_.size() < kMaxActors) {
        for (const Entity2D& t : authored_) {
          if (t.name != r.name) continue;
          Entity2D data = t;
          data.player.reset();
          data.camera.reset();
          data.spawn.reset();
          data.id = r.id;
          normalize_components(data);
          Actor n;
          n.data = std::move(data);
          n.spawned = true;
          setup_actor_animation(n);
          actors_.push_back(std::move(n));
          next_id_ = std::max(next_id_, r.id + 1);
          dirty_ = true;
          made = r.id;
          break;
        }
      }
      a = made ? live(made) : nullptr;
      if (!a) continue;
    }
    a->data.x = r.x;
    a->data.y = r.y;
    a->data.rotation = r.rotation;
    a->prev_x = r.x;
    a->prev_y = r.y;
    a->in_x = r.in_x;
    a->in_y = r.in_y;
    a->facing = r.facing < 0 ? -1 : 1;
    a->dir = r.dir;
    a->hidden = r.hidden;
    if (r.collider_on != a->data.collider.has_value()) {
      script_set_collider(r.id, r.collider_on);
    }
  }
  for (Actor& a : actors_) {
    if (!a.dead && seen.count(a.data.id) == 0) {
      a.dead = true;
      dirty_ = true;
    }
  }
  apply_changes();
}

void World::net_animate() {
  step_animation();
  ++tick_;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
