#include "world3d/World3D.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace ts {
namespace tombstone {
namespace world3d {

using render3d::Mat4;
using render3d::Quat;
using runtime::LogEntry;
using runtime::LogLevel;

namespace {

Vec3 closest_on_segment(const Vec3& a, const Vec3& b, const Vec3& p) {
  const Vec3 ab = b - a;
  const float l2 = render3d::dot(ab, ab);
  if (l2 <= 1e-12f) return a;
  const float t = std::clamp(render3d::dot(p - a, ab) / l2, 0.0f, 1.0f);
  return a + ab * t;
}

Vec3 closest_on_box(const Aabb& b, const Vec3& p) {
  return {std::clamp(p.x, b.min.x, b.max.x), std::clamp(p.y, b.min.y, b.max.y),
          std::clamp(p.z, b.min.z, b.max.z)};
}

bool overlap(const Aabb& a, const Aabb& b) {
  return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y &&
         a.min.z < b.max.z && a.max.z > b.min.z;
}

}  // namespace

Vec3 capsule_box_push(const Vec3& a, const Vec3& b, float r, const Aabb& box) {
  // Alternate closest points a few times (segment <-> box); converges for
  // convex shapes well enough for a character controller.
  Vec3 s = closest_on_segment(a, b, box.center());
  Vec3 q = closest_on_box(box, s);
  for (int i = 0; i < 3; ++i) {
    s = closest_on_segment(a, b, q);
    q = closest_on_box(box, s);
  }
  const Vec3 d = s - q;
  const float dist = render3d::length(d);
  if (dist >= r) return {};
  if (dist > 1e-6f) return d * ((r - dist) / dist);
  // Segment point inside the box: leave along the shallowest face.
  float best = 1e30f;
  Vec3 push;
  for (int axis = 0; axis < 3; ++axis) {
    const float lo = s[axis] - box.min[axis] + r;
    const float hi = box.max[axis] - s[axis] + r;
    if (lo < best) {
      best = lo;
      push = {};
      push[axis] = -lo;
    }
    if (hi < best) {
      best = hi;
      push = {};
      push[axis] = hi;
    }
  }
  return push;
}

World3D::World3D() = default;
World3D::~World3D() { stop(); }

void World3D::post(LogLevel level, const std::string& channel, const std::string& text,
                   std::uint64_t entity) {
  LogEntry e;
  e.level = level;
  e.channel = channel;
  e.text = text;
  e.entity = entity;
  e.tick = script_tick();
  script_log(std::move(e));
}

void World3D::script_log(LogEntry entry) {
  if (entry.tick == 0) entry.tick = script_tick();
  if (sink_) sink_(entry);
  log_.push_back(std::move(entry));
  while (log_.size() > 500) log_.pop_front();
}

bool World3D::start(const scene3d::Scene3D& scene, const std::string& project_dir,
                    std::string* error) {
  stop();
  authored_ = scene;
  ents_ = scene.entities;
  has_parents_ = std::any_of(ents_.begin(), ents_.end(),
                             [](const Entity3D& e) { return e.parent != 0; });
  project_dir_ = project_dir;
  tick_ = 0;
  accumulator_ = 0.0;
  vel_ = {};
  grounded_ = false;
  paused_ = false;
  trigger_events_ = 0;
  player_ = 0;
  for (const Entity3D& e : ents_) {
    if (e.player) {
      player_ = e.id;
      break;
    }
  }
  if (vr_) vr_start();
  if (player_ == 0) {
    post(LogLevel::Warn, "play",
         "No rider in this scene (give an entity a Player component); the camera stays put");
  }
  if (const Entity3D* p = find(player_)) spawn_ = p->transform.position;
  rebuild_statics();
  // Camera starts behind the rider, looking the way the editor camera did.
  cam_yaw_ = scene.editor_camera.yaw;
  camera_.position = {0, 3, 8};
  camera_.target = {0, 1, 0};
  follow_camera(true);
  if (vr_) {
    vr_update_poses();
  } else if (!player_) {
    const float yaw = render3d::radians(scene.editor_camera.yaw);
    const float pitch = render3d::radians(scene.editor_camera.pitch);
    const float d = scene.editor_camera.distance;
    camera_.target = scene.editor_camera.target;
    camera_.position = camera_.target + Vec3{std::sin(yaw) * std::cos(pitch) * d,
                                             std::sin(pitch) * d,
                                             std::cos(yaw) * std::cos(pitch) * d};
  }
  running_ = true;
  // Scripts.
  bool any_script = false;
  for (const Entity3D& e : ents_) any_script = any_script || e.script.has_value();
  if (any_script) {
    host_ = std::make_unique<runtime::ScriptHost>(*this, lib_, project_dir_);
    for (const Entity3D& e : ents_) {
      if (!e.script) continue;
      host_->attach(*script_entity(e.id));
    }
  }
  post(LogLevel::Info, "play",
       "Riding " + std::to_string(ents_.size()) + " entities, " +
           std::to_string(statics_.size()) + " static colliders, " +
           std::to_string(script_count()) + " script(s)");
  (void)error;
  return true;
}

void World3D::stop() {
  host_.reset();
  running_ = false;
  ents_.clear();
  proxies_.clear();
  statics_.clear();
  inside_.clear();
  spawned_.clear();
  collider_off_.clear();
  dead_.clear();
  state_.clear();
  toasts_.clear();
  sounds_.clear();
  bodies_.clear();
  grab_home_.clear();
  haptics_.clear();
  for (VrHandState& h : hands_) h = VrHandState{};
  tele_ = VrTeleport{};
  menu_open_ = false;
  menu_.clear();
  rig_id_ = 0;
}

std::size_t World3D::script_count() const { return host_ ? host_->instance_count() : 0; }

const Entity3D* World3D::find(std::uint64_t id) const {
  for (const Entity3D& e : ents_) {
    if (e.id == id) return &e;
  }
  return nullptr;
}

Entity3D* World3D::live(std::uint64_t id) {
  if (id == 0 || dead_.count(id)) return nullptr;
  for (Entity3D& e : ents_) {
    if (e.id == id) return &e;
  }
  return nullptr;
}

Aabb World3D::collider_box(const Entity3D& e) const {
  const scene3d::Collider3D& c = *e.collider;
  Aabb local;
  if (c.shape == scene3d::Collider3D::Shape::Capsule) {
    const float r = c.size.x, h = std::max(c.size.y, 2.0f * c.size.x);
    local = {{-r, -h * 0.5f, -r}, {r, h * 0.5f, r}};
  } else {
    local = {c.size * -0.5f, c.size * 0.5f};
  }
  local.min += c.offset;
  local.max += c.offset;
  return local.transformed(e.transform.matrix());
}

void World3D::rebuild_statics() {
  statics_.clear();
  for (const Entity3D& e : ents_) {
    if (!e.collider || e.collider->trigger || e.player || e.grabbable || e.vr_rig ||
        dead_.count(e.id) ||
        collider_off_.count(e.id)) {
      continue;
    }
    statics_.push_back({e.id, collider_box(e)});
  }
}

void World3D::follow_camera(bool snap) {
  const Entity3D* p = find(player_);
  if (!p) return;
  const scene3d::Player3D cfg = p->player ? *p->player : scene3d::Player3D{};
  const float yaw = render3d::radians(cam_yaw_);
  const Vec3 target = p->transform.position + Vec3{0, 1.0f, 0};
  Vec3 want = p->transform.position +
              Vec3{std::sin(yaw) * cfg.camera_distance, cfg.camera_height,
                   std::cos(yaw) * cfg.camera_distance};
  // Pull in when a static box sits between the rider and the camera.
  const render3d::Ray ray{target, render3d::normalize(want - target)};
  const float full = render3d::length(want - target);
  float best = full;
  for (const Static& s : statics_) {
    float t = 0.0f;
    if (render3d::ray_aabb(ray, s.box, &t) && t > 0.3f && t < best) best = t;
  }
  if (best < full) want = target + ray.dir * std::max(0.8f, best - 0.3f);
  if (snap) {
    camera_.position = want;
    camera_.target = target;
  } else {
    camera_.position = render3d::lerp(camera_.position, want, 0.18f);
    camera_.target = render3d::lerp(camera_.target, target, 0.3f);
  }
}

void World3D::move_player(const Input3D& in) {
  contacts_ = 0;
  Entity3D* p = live(player_);
  if (!p) return;
  const scene3d::Player3D cfg = p->player ? *p->player : scene3d::Player3D{};
  cam_yaw_ += in.turn;
  const float yaw = render3d::radians(cam_yaw_);
  const Vec3 fwd{-std::sin(yaw), 0.0f, -std::cos(yaw)};
  const Vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
  Vec3 wish = fwd * std::clamp(in.move_forward, -1.0f, 1.0f) +
              right * std::clamp(in.move_right, -1.0f, 1.0f);
  if (render3d::length(wish) > 1.0f) wish = render3d::normalize(wish);
  const float dt = static_cast<float>(kTickSeconds);
  // Ground control snaps, air control eases.
  const float accel = grounded_ ? 1.0f : 0.15f;
  vel_.x += (wish.x * cfg.speed - vel_.x) * accel;
  vel_.z += (wish.z * cfg.speed - vel_.z) * accel;
  if (render3d::length(wish) > 0.05f) {
    p->transform.rotation = Quat::axis_angle({0, 1, 0}, std::atan2(-wish.x, -wish.z));
  }
  if (in.jump && grounded_) {
    vel_.y = cfg.jump_speed;
    grounded_ = false;
  }
  vel_.y -= kGravity * dt;
  p->transform.position += vel_ * dt;

  // Rider capsule from its collider (default 0.4 x 2.0).
  float r = 0.4f, h = 2.0f;
  Vec3 off;
  if (p->collider) {
    r = p->collider->shape == scene3d::Collider3D::Shape::Capsule ? p->collider->size.x
                                                                  : 0.5f * std::max(p->collider->size.x, p->collider->size.z);
    h = p->collider->size.y;
    off = p->collider->offset;
  }
  const float half = std::max(0.0f, h * 0.5f - r);
  grounded_ = false;
  for (int iter = 0; iter < 4; ++iter) {
    bool moved = false;
    for (const Static& s : statics_) {
      const Vec3 c = p->transform.position + off;
      const Vec3 push = capsule_box_push(c - Vec3{0, half, 0}, c + Vec3{0, half, 0}, r, s.box);
      const float len = render3d::length(push);
      if (len <= 1e-6f) continue;
      ++contacts_;
      moved = true;
      p->transform.position += push;
      const Vec3 n = push / len;
      if (n.y > 0.6f) {
        grounded_ = true;
        if (vel_.y < 0) vel_.y = 0;
      } else if (n.y < -0.6f) {
        if (vel_.y > 0) vel_.y = 0;
      }
      // Slide: drop the velocity into the wall.
      const float into = render3d::dot(vel_, n);
      if (into < 0.0f && std::fabs(n.y) <= 0.6f) vel_ -= n * into;
    }
    if (!moved) break;
  }
  if (p->transform.position.y < kKillY) {
    p->transform.position = spawn_;
    vel_ = {};
    post(LogLevel::Warn, "play", p->name + " fell off the edge of the map; back to the spawn", p->id);
  }
}

void World3D::update_triggers() {
  const Entity3D* p = find(player_);
  if (!p || dead_.count(player_)) return;
  float r = 0.4f, h = 2.0f;
  Vec3 off;
  if (p->collider) {
    r = p->collider->size.x;
    h = p->collider->size.y;
    off = p->collider->offset;
  }
  const Vec3 c = p->transform.position + off;
  const Aabb me{c - Vec3{r, h * 0.5f, r}, c + Vec3{r, h * 0.5f, r}};
  std::set<std::uint64_t> now;
  for (const Entity3D& e : ents_) {
    if (!e.collider || !e.collider->trigger || dead_.count(e.id) || collider_off_.count(e.id)) continue;
    if (overlap(me, collider_box(e))) now.insert(e.id);
  }
  for (std::uint64_t id : now) {
    if (inside_.count(id)) continue;
    ++trigger_events_;
    const Entity3D* t = find(id);
    post(LogLevel::Info, "trigger", p->name + " rode into " + (t ? t->name : "?"), id);
    if (host_) host_->trigger(true, id, player_);
  }
  for (std::uint64_t id : inside_) {
    if (now.count(id)) continue;
    ++trigger_events_;
    if (host_) host_->trigger(false, id, player_);
  }
  inside_ = std::move(now);
}

void World3D::interact() {
  const Entity3D* p = find(player_);
  if (!p || !host_) return;
  std::uint64_t best = 0;
  float best_d = kInteractReach;
  for (const Entity3D& e : ents_) {
    if (e.id == player_ || !e.script || dead_.count(e.id)) continue;
    if (!host_->running(e.id) || !host_->has_hook(e.id, "on_interact")) continue;
    const float d = render3d::length(e.transform.position - p->transform.position);
    if (d < best_d) {
      best_d = d;
      best = e.id;
    }
  }
  if (best) host_->interact(best, player_);
}

void World3D::follow_parents(const std::vector<std::pair<std::uint64_t, Transform>>& before) {
  std::vector<std::size_t> order(ents_.size());
  std::vector<int> depth(ents_.size());
  for (std::size_t i = 0; i < ents_.size(); ++i) {
    order[i] = i;
    depth[i] = scene3d::depth_of(ents_, ents_[i].id);
  }
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) { return depth[a] < depth[b]; });
  auto prev_of = [&](std::uint64_t id) -> const Transform* {
    for (const auto& p : before) {
      if (p.first == id) return &p.second;
    }
    return nullptr;
  };
  for (std::size_t i : order) {
    const Entity3D& par = ents_[i];
    const Transform* prev = prev_of(par.id);
    if (!prev || *prev == par.transform) continue;
    const Transform now = par.transform;
    const Transform was = *prev;
    for (Entity3D& c : ents_) {
      if (c.parent == par.id) scene3d::carry_child(&c, was, now);
    }
  }
}

void World3D::step(const Input3D& input0) {
  if (!running_) return;
  Input3D input = input0;
  input.jump = input.jump || jump_latched_;
  input.action = input.action || action_latched_;
  jump_latched_ = action_latched_ = false;
  in_step_ = true;
  std::vector<std::pair<std::uint64_t, Transform>> before;
  if (has_parents_) {
    before.reserve(ents_.size());
    for (const Entity3D& e : ents_) before.emplace_back(e.id, e.transform);
  }
  if (vr_) {
    vr_step();
  } else {
    move_player(input);
    update_triggers();
  }
  if (host_) {
    host_->start_pending();
    if (input.action && !jump_held_) interact();
    host_->run_timers(script_tick());
    host_->tick(kTickSeconds);
  }
  jump_held_ = input.action;
  ++tick_;
  in_step_ = false;
  if (has_parents_) follow_parents(before);
  // Scripts may have moved or switched colliders.
  rebuild_statics();
  if (!vr_) follow_camera(false);
}

int World3D::update(double real_seconds, const Input3D& input) {
  if (!running_ || paused_) return 0;
  accumulator_ += std::min(real_seconds, 0.25);
  int n = 0;
  jump_latched_ = jump_latched_ || input.jump;
  action_latched_ = action_latched_ || input.action;
  while (accumulator_ >= kTickSeconds && n < kMaxCatchUpTicks) {
    accumulator_ -= kTickSeconds;
    step(input);
    ++n;
  }
  if (n == kMaxCatchUpTicks) accumulator_ = 0.0;
  return n;
}

void World3D::run_ticks(int n, const Input3D& input) {
  for (int i = 0; i < n; ++i) step(input);
}

int World3D::reload_scripts() { return host_ ? host_->reload_changed() : 0; }

std::vector<Toast3D> World3D::take_toasts() {
  std::vector<Toast3D> out;
  out.swap(toasts_);
  return out;
}

const ScriptValue* World3D::state(std::uint64_t id, const std::string& key) const {
  auto it = state_.find(id);
  if (it == state_.end()) return nullptr;
  auto kv = it->second.find(key);
  return kv == it->second.end() ? nullptr : &kv->second;
}

// ---- ScriptWorld ---------------------------------------------------------

std::uint64_t World3D::script_tick() const { return in_step_ ? tick_ + 1 : tick_; }

const Entity2D* World3D::script_entity(std::uint64_t id) const {
  if (dead_.count(id)) return nullptr;
  const Entity3D* e = find(id);
  if (!e) return nullptr;
  Entity2D& p = proxies_[id];
  p.id = e->id;
  p.name = e->name;
  p.x = e->transform.position.x;
  p.y = e->transform.position.z;
  p.w = e->transform.scale.x;
  p.h = e->transform.scale.z;
  if (e->script) p.script = e->script;
  if (e->player) {
    PlayerControllerData pc;
    p.player = pc;
  }
  return &p;
}

std::uint64_t World3D::script_find(const std::string& name) const {
  for (const Entity3D& e : ents_) {
    if (e.name == name && !dead_.count(e.id)) return e.id;
  }
  return 0;
}

std::uint64_t World3D::script_player(int slot) const { return slot == 0 ? player_ : 0; }

void World3D::script_move(std::uint64_t id, float x, float y) {
  if (Entity3D* e = live(id)) {
    e->transform.position.x = x;
    e->transform.position.z = y;
  }
}

bool World3D::script_visible(std::uint64_t id) const {
  const Entity3D* e = find(id);
  return e && e->visible && !dead_.count(id);
}

void World3D::script_set_visible(std::uint64_t id, bool on) {
  if (Entity3D* e = live(id)) e->visible = on;
}

bool World3D::script_collider_on(std::uint64_t id) const {
  const Entity3D* e = find(id);
  return e && e->collider && !collider_off_.count(id) && !dead_.count(id);
}

void World3D::script_set_collider(std::uint64_t id, bool on) {
  Entity3D* e = live(id);
  if (!e || !e->collider) return;
  if (on) collider_off_.erase(id);
  else collider_off_.insert(id);
  rebuild_statics();
}

bool World3D::script_play(std::uint64_t /*id*/, const std::string& /*clip*/, bool /*hold*/) {
  return false;  // no 3D animation clips yet
}

void World3D::script_release(std::uint64_t /*id*/) {}

std::uint64_t World3D::spawn_copy(const Entity3D& src, float x, float z, bool keep_y, float y) {
  if (ents_.size() >= 4096) {
    post(LogLevel::Warn, "script", "Too many entities; spawn refused");
    return 0;
  }
  std::uint64_t id = 1;
  for (const Entity3D& e : ents_) id = std::max(id, e.id + 1);
  Entity3D c = src;
  c.id = id;
  c.player.reset();
  c.transform.position = {x, keep_y ? src.transform.position.y : y, z};
  ents_.push_back(c);
  spawned_.insert(id);
  rebuild_statics();
  if (host_ && c.script) host_->attach(*script_entity(id));
  return id;
}

std::uint64_t World3D::script_spawn(const std::string& tmpl, float x, float y) {
  const Entity3D* src = authored_.find_named(tmpl);
  if (!src) return 0;
  const Entity3D copy = *src;
  return spawn_copy(copy, x, y, true, 0.0f);
}

std::uint64_t World3D::script_duplicate(std::uint64_t id, float x, float y) {
  const Entity3D* src = live(id);
  if (!src) return 0;
  const Entity3D copy = *src;
  return spawn_copy(copy, x, y, true, 0.0f);
}

void World3D::script_destroy(std::uint64_t id) {
  if (!live(id)) return;
  if (id == player_) {
    post(LogLevel::Warn, "script", "Scripts cannot destroy the rider");
    return;
  }
  dead_.insert(id);
  if (Entity3D* e = live(id)) e->visible = false;
  for (Entity3D& e : ents_) {
    if (e.id == id) e.visible = false;
  }
  if (host_) host_->detach(id);
  rebuild_statics();
}

bool World3D::script_spawned(std::uint64_t id) const { return spawned_.count(id) != 0; }

const ScriptValue* World3D::script_get(std::uint64_t id, const std::string& key) const {
  return state(id, key);
}

void World3D::script_set(std::uint64_t id, const std::string& key, const ScriptValue* value) {
  if (!find(id)) return;
  if (!value) {
    state_[id].erase(key);
    return;
  }
  auto& m = state_[id];
  if (m.size() >= runtime::ScriptHost::kMaxStateKeys && !m.count(key)) return;
  m[key] = *value;
}

void World3D::script_toast(int /*slot*/, const std::string& text, double seconds,
                           std::uint64_t from) {
  toasts_.push_back({text, seconds, from});
  post(LogLevel::Info, "toast", text, from);
}

void World3D::script_sound(runtime::SoundEvent event) { play_sound(std::move(event)); }

void World3D::play_sound(runtime::SoundEvent event) {
  event.three_d = true;
  if (event.positional && event.entity != 0) {
    if (const Entity3D* e = find(event.entity)) {
      event.x = e->transform.position.x;
      event.y = e->transform.position.y;
      event.z = e->transform.position.z;
    }
  }
  if (sounds_.size() >= kMaxSounds) sounds_.erase(sounds_.begin());
  sounds_.push_back(std::move(event));
}

std::vector<runtime::SoundEvent> World3D::take_sounds() {
  std::vector<runtime::SoundEvent> out;
  out.swap(sounds_);
  return out;
}

void World3D::listener_pose(Vec3* position, Vec3* forward, Vec3* up) const {
  if (vr_ && head_.valid) {
    *position = head_.position;
    *forward = head_.forward();
    *up = head_.orientation.rotate({0.0f, 1.0f, 0.0f});
    return;
  }
  *position = camera_.position;
  *forward = camera_.forward();
  *up = camera_.up;
}

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
