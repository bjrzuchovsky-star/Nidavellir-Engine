// The world side of gameplay scripts: building the script host, the
// script phase of a tick (triggers, action button, timers, on_tick), and
// everything ScriptWorld lets a script do (move, hide, collider toggles,
// spawn / destroy, per-entity state, toasts, Telegraph lines).

#include "core/MemImages.h"
#include "runtime/World.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ts {
namespace tombstone {
namespace runtime {

static_assert(kScriptTickRate == kTickRate,
              "scripts must tick with the world");

namespace {

constexpr std::size_t kMaxToasts = 32;
constexpr std::size_t kMaxToastText = 200;

}  // namespace

World::~World() {
  if (!lightmap_key_.empty()) mem_images::remove(lightmap_key_);
}

Actor* World::live(std::uint64_t id) {
  Actor* a = find(id);
  return a && !a->dead ? a : nullptr;
}

const Actor* World::live(std::uint64_t id) const {
  const Actor* a = find(id);
  return a && !a->dead ? a : nullptr;
}

void World::post(LogLevel level, const std::string& channel, std::string text,
                 std::uint64_t entity) {
  LogEntry e;
  e.level = level;
  e.channel = channel;
  e.text = std::move(text);
  e.entity = entity;
  e.tick = script_tick();
  script_log(std::move(e));
}

void World::setup_scripts() {
  authored_.clear();
  authored_.reserve(actors_.size());
  for (const Actor& a : actors_) {
    authored_.push_back(a.data);
    next_id_ = std::max(next_id_, a.data.id + 1);
  }
  const bool any = std::any_of(actors_.begin(), actors_.end(),
                               [](const Actor& a) { return a.data.script.has_value(); });
  if (!any) {
    return;
  }
  scripts_ = std::make_unique<ScriptHost>(*this, script_lib_, project_dir_);
  // Top levels run in scene order; on_start waits for the first tick.
  // Collect ids first: a top level may spawn.
  std::vector<std::uint64_t> ids;
  for (const Actor& a : actors_) {
    if (a.data.script) ids.push_back(a.data.id);
  }
  for (std::uint64_t id : ids) {
    if (const Actor* a = live(id)) {
      const Entity2D copy = a->data;
      scripts_->attach(copy);
    }
  }
  apply_changes();
}

void World::step_scripts(const InputFrame& input) {
  if (scripts_) {
    for (const TriggerEvent& ev : events_) {
      scripts_->trigger(ev.kind == TriggerEvent::Kind::Enter, ev.trigger,
                        ev.other);
    }
    // Action button: once per press (rising edge), on the nearest
    // interactable within reach.
    for (int slot = 0; slot < kMaxPlayers; ++slot) {
      const std::uint32_t now = input.slot(slot).buttons;
      const std::uint32_t before = prev_buttons_[static_cast<std::size_t>(slot)];
      if ((now & kButtonAction) == 0 || (before & kButtonAction) != 0) {
        continue;
      }
      const Actor* p = player(slot);
      if (!p || p->dead) {
        continue;
      }
      const std::uint64_t rider = p->data.id;
      if (const std::uint64_t target = interact_target(rider)) {
        scripts_->interact(target, rider);
      }
    }
    scripts_->run_timers(script_tick());
    scripts_->tick(kTickSeconds);
  }
  for (int slot = 0; slot < kMaxPlayers; ++slot) {
    prev_buttons_[static_cast<std::size_t>(slot)] = input.slot(slot).buttons;
  }
  apply_changes();
}

std::uint64_t World::interact_target(std::uint64_t rider) {
  const Actor* r = live(rider);
  if (!r || !scripts_) {
    return 0;
  }
  const Aabb rb = collider_box(r->data);
  const float rcx = (rb.x0 + rb.x1) * 0.5f;
  const float rcy = (rb.y0 + rb.y1) * 0.5f;
  std::vector<std::uint64_t> near;
  std::vector<std::pair<float, float>> score;  // (gap, centre distance^2)
  for (const Actor& a : actors_) {
    const Entity2D& e = a.data;
    if (a.dead || a.hidden || e.id == rider || !e.script) {
      continue;
    }
    // Reach is measured to the entity rect, so an opened gate (collider
    // off) can still be closed again.
    const float gx = std::max(e.x - rb.x1, rb.x0 - (e.x + e.w));
    const float gy = std::max(e.y - rb.y1, rb.y0 - (e.y + e.h));
    const float gap = std::max(0.0f, std::max(gx, gy));
    if (gap > kInteractReach) {
      continue;
    }
    const float dx = e.center_x() - rcx;
    const float dy = e.center_y() - rcy;
    near.push_back(e.id);
    score.emplace_back(gap, dx * dx + dy * dy);
  }
  std::uint64_t best = 0;
  std::pair<float, float> best_score{std::numeric_limits<float>::max(), 0.0f};
  for (std::size_t i = 0; i < near.size(); ++i) {
    if (!scripts_->running(near[i]) || !scripts_->has_hook(near[i], "on_interact")) {
      continue;
    }
    if (best == 0 || score[i] < best_score ||
        (score[i] == best_score && near[i] < best)) {
      best = near[i];
      best_score = score[i];
    }
  }
  return best;
}

void World::apply_changes() {
  if (!dirty_) {
    return;
  }
  dirty_ = false;
  std::vector<std::uint64_t> gone;
  for (const Actor& a : actors_) {
    if (a.dead) gone.push_back(a.data.id);
  }
  for (std::uint64_t id : gone) {
    if (scripts_) scripts_->detach(id);
  }
  if (!gone.empty()) {
    actors_.erase(std::remove_if(actors_.begin(), actors_.end(),
                                 [](const Actor& a) { return a.dead; }),
                  actors_.end());
    // The destroyed never "ride out": forget their trigger pairs quietly.
    auto is_gone = [&](std::uint64_t id) {
      return std::find(gone.begin(), gone.end(), id) != gone.end();
    };
    inside_.erase(std::remove_if(inside_.begin(), inside_.end(),
                                 [&](const auto& p) {
                                   return is_gone(p.first) || is_gone(p.second);
                                 }),
                  inside_.end());
  }
  // Re-index collision and draw order; this tick's trigger record stays.
  auto inside = std::move(inside_);
  auto events = std::move(events_);
  const auto last = last_event_;
  const std::uint64_t total = events_total_;
  setup_collision();
  inside_ = std::move(inside);
  events_ = std::move(events);
  last_event_ = last;
  events_total_ = total;
  std::vector<Entity2D> plain;
  plain.reserve(actors_.size());
  for (const Actor& a : actors_) {
    plain.push_back(a.data);
  }
  draw_order_ = draw_order(plain);
}

int World::reload_scripts() {
  return scripts_ ? scripts_->reload_changed() : 0;
}

std::vector<LogEntry> World::take_logs() {
  std::vector<LogEntry> out = std::move(logs_);
  logs_.clear();
  return out;
}

std::vector<const Toast*> World::toasts_for(int slot) const {
  std::vector<const Toast*> out;
  for (const Toast& t : toasts_) {
    if (t.slot < 0 || t.slot == slot) out.push_back(&t);
  }
  return out;
}

// --- ScriptWorld -----------------------------------------------------------------

std::uint64_t World::script_tick() const { return in_step_ ? tick_ + 1 : tick_; }

const Entity2D* World::script_entity(std::uint64_t id) const {
  const Actor* a = live(id);
  return a ? &a->data : nullptr;
}

std::uint64_t World::script_find(const std::string& name) const {
  for (const Actor& a : actors_) {
    if (!a.dead && a.data.name == name) return a.data.id;
  }
  return 0;
}

std::uint64_t World::script_player(int slot) const {
  const Actor* p = player(slot);
  return p && !p->dead ? p->data.id : 0;
}

void World::script_move(std::uint64_t id, float x, float y) {
  if (Actor* a = live(id)) {
    a->data.x = x;
    a->data.y = y;
  }
}

bool World::script_visible(std::uint64_t id) const {
  const Actor* a = live(id);
  return a && !a->hidden;
}

void World::script_set_visible(std::uint64_t id, bool on) {
  if (Actor* a = live(id)) a->hidden = !on;
}

bool World::script_collider_on(std::uint64_t id) const {
  const Actor* a = live(id);
  return a && a->data.collider.has_value();
}

void World::script_set_collider(std::uint64_t id, bool on) {
  Actor* a = live(id);
  if (!a) {
    return;
  }
  if (on && !a->data.collider && a->stashed) {
    a->data.collider = std::move(a->stashed);
    a->stashed.reset();
    dirty_ = true;
  } else if (!on && a->data.collider) {
    a->stashed = std::move(a->data.collider);
    a->data.collider.reset();
    dirty_ = true;
  }
}

bool World::script_play(std::uint64_t id, const std::string& clip, bool hold) {
  Actor* a = live(id);
  if (!a) {
    return false;
  }
  // Already on it: keep going (a script may ask every tick); a finished
  // once-clip starts over.
  const bool restart = a->anim.clip != clip || a->anim.finished;
  return play_clip(id, clip, restart, hold);
}

void World::script_release(std::uint64_t id) {
  if (live(id)) release_clip(id);
}

std::uint64_t World::add_actor(Entity2D data) {
  if (actors_.size() >= kMaxActors) {
    post(LogLevel::Warn, "script",
         "The world is full (" + std::to_string(kMaxActors) +
             " entities); spawn refused",
         0);
    return 0;
  }
  // Copies carry looks, collider, animator and script, not rider / camera
  // / spawn roles.
  data.player.reset();
  data.camera.reset();
  data.spawn.reset();
  data.id = next_id_++;
  normalize_components(data);
  Actor a;
  a.data = std::move(data);
  a.prev_x = a.data.x;
  a.prev_y = a.data.y;
  a.spawned = true;
  setup_actor_animation(a);
  const std::uint64_t id = a.data.id;
  actors_.push_back(std::move(a));
  dirty_ = true;
  if (scripts_ && actors_.back().data.script) {
    const Entity2D copy = actors_.back().data;
    scripts_->attach(copy);
  }
  return id;
}

std::uint64_t World::script_spawn(const std::string& tmpl, float x, float y) {
  const Entity2D* t = find_entity_named(authored_, tmpl);
  if (!t) {
    post(LogLevel::Warn, "script", "spawn: no entity named \"" + tmpl + "\" in the scene", 0);
    return 0;
  }
  Entity2D data = *t;
  data.x = x;
  data.y = y;
  return add_actor(std::move(data));
}

std::uint64_t World::script_duplicate(std::uint64_t id, float x, float y) {
  const Actor* src = live(id);
  if (!src) {
    return 0;
  }
  Entity2D data = src->data;
  if (!data.collider && src->stashed) {
    data.collider = src->stashed;
  }
  data.x = x;
  data.y = y;
  return add_actor(std::move(data));
}

void World::script_destroy(std::uint64_t id) {
  if (Actor* a = live(id)) {
    a->dead = true;
    dirty_ = true;
  }
}

bool World::script_spawned(std::uint64_t id) const {
  const Actor* a = live(id);
  return a && a->spawned;
}

const ScriptValue* World::script_get(std::uint64_t id,
                                     const std::string& key) const {
  const Actor* a = live(id);
  if (!a) {
    return nullptr;
  }
  const auto it = a->state.find(key);
  return it == a->state.end() ? nullptr : &it->second;
}

void World::script_set(std::uint64_t id, const std::string& key,
                       const ScriptValue* value) {
  Actor* a = live(id);
  if (!a) {
    return;
  }
  if (!value) {
    a->state.erase(key);
    return;
  }
  const auto it = a->state.find(key);
  if (it != a->state.end()) {
    it->second = *value;
  } else if (a->state.size() < ScriptHost::kMaxStateKeys) {
    a->state.emplace(key, *value);
  } else {
    post(LogLevel::Warn, "script",
         a->data.name + " holds " + std::to_string(ScriptHost::kMaxStateKeys) +
             " state keys already; \"" + key + "\" not kept",
         id);
  }
}

void World::script_toast(int slot, const std::string& text, double seconds,
                         std::uint64_t from) {
  Toast t;
  t.slot = slot;
  t.text = text.substr(0, kMaxToastText);
  const double sec = std::isfinite(seconds) ? std::clamp(seconds, 0.5, 30.0) : 2.5;
  t.until = script_tick() + static_cast<std::uint64_t>(std::llround(sec * kTickRate));
  t.entity = from;
  post(LogLevel::Info, "toast",
       (slot < 0 ? std::string("all riders: ") : "P" + std::to_string(slot + 1) + ": ") +
           t.text,
       from);
  toasts_.push_back(std::move(t));
  if (toasts_.size() > kMaxToasts) {
    toasts_.erase(toasts_.begin());
  }
}

void World::script_log(LogEntry entry) {
  if (logs_.size() >= kMaxPendingLogs) {
    // Nobody is draining: keep the newest half.
    logs_.erase(logs_.begin(), logs_.begin() + static_cast<std::ptrdiff_t>(kMaxPendingLogs / 2));
  }
  ++log_total_;
  logs_.push_back(std::move(entry));
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts

namespace ts {
namespace tombstone {
namespace runtime {

std::uint32_t World::script_buttons(int slot) const {
  if (slot < 0 || slot >= kMaxPlayers) return 0;
  return input_.slot(slot).buttons;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
