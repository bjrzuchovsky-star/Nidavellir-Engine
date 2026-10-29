#include "core/Localization.h"
#include "profiler/Profiler.h"
#include "runtime/World.h"
#include "runtime/RiderController2D.h"

#include "scene/SceneJson.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

namespace ts {
namespace tombstone {
namespace runtime {

namespace fs = std::filesystem;

std::string resolve_asset(const std::string& project_dir,
                          const std::string& rel_path) {
  if (rel_path.empty()) {
    return {};
  }
  if (rel_path.rfind("mem:", 0) == 0) {
    return rel_path;  // core/MemImages key (UI atlas, rasters)
  }
  const fs::path p = fs::path(rel_path);
  if (p.is_absolute() || project_dir.empty()) {
    return p.string();
  }
  return (fs::path(project_dir) / p).string();
}

bool World::build(const std::vector<Entity2D>& entities,
                  const std::string& project_dir, std::string* error_out) {
  return build(entities, TileSolidity{}, project_dir, error_out);
}

bool World::build(const std::vector<Entity2D>& entities,
                  const TileSolidity& solidity,
                  const std::string& project_dir, std::string* error_out) {
  clear();
  solidity_ = solidity;
  project_dir_ = project_dir;
  // An empty scene rides too (a Truly Blank claim): no actors, the camera
  // stays at the origin and ticks still count.
  (void)error_out;
  actors_.reserve(entities.size());
  for (const Entity2D& e : entities) {
    Actor a;
    a.data = e;
    normalize_components(a.data);
    actors_.push_back(std::move(a));
  }
  // Players start on their slot's SpawnPoint when there is one.
  for (Actor& a : actors_) {
    if (!a.data.player) {
      continue;
    }
    for (const Actor& s : actors_) {
      if (s.data.spawn && s.data.spawn->slot == a.data.player->slot) {
        a.data.x = s.data.center_x() - a.data.w * 0.5f;
        a.data.y = s.data.center_y() - a.data.h * 0.5f;
        break;
      }
    }
  }
  for (Actor& a : actors_) {
    a.prev_x = a.data.x;
    a.prev_y = a.data.y;
  }
  std::vector<Entity2D> plain;
  plain.reserve(actors_.size());
  for (const Actor& a : actors_) {
    plain.push_back(a.data);
  }
  draw_order_ = draw_order(plain);
  setup_collision();
  setup_animation();
  setup_camera();
  setup_scripts();
  return true;
}

bool World::load_project(const std::string& project_dir,
                         std::string* error_out) {
  loc::load_project(project_dir);  // tr() / lang.* in scripts
  scene_json::SceneDoc doc;
  if (!scene_json::load_file(scene_json::scene_path_for_project(project_dir),
                             &doc, error_out)) {
    return false;
  }
  if (!build(doc.entities, doc.tile_solidity, project_dir, error_out)) return false;
  set_lighting(doc.lighting);
  return true;
}

void World::clear() {
  scripts_.reset();
  ext_dead_.assign(ext_ ? ext_->systems.size() : 0, false);
  authored_.clear();
  logs_.clear();
  toasts_.clear();
  sounds_.clear();
  ui_commands_.clear();
  particles_.clear();
  lighting_ = fx::Lighting2D{};
  fx_total_ = 0;
  lightmap_ = fx::Lightmap{};
  next_id_ = 1;
  prev_buttons_ = {};
  in_step_ = false;
  dirty_ = false;
  actors_.clear();
  draw_order_.clear();
  cam_ = CameraRig{};
  tick_ = 0;
  rider_warned_ = 0;
  phys2d_warned_ = false;
  project_dir_.clear();
  solidity_ = TileSolidity{};
  grids_.clear();
  static_solids_.clear();
  dynamics_.clear();
  triggers_.clear();
  step_limit_ = 1.0e9f;
  inside_.clear();
  events_.clear();
  last_event_.reset();
  events_total_ = 0;
  anim_sets_.clear();
  anim_paths_.clear();
}

Actor* World::find(std::uint64_t id) {
  for (Actor& a : actors_) {
    if (a.data.id == id) {
      return &a;
    }
  }
  return nullptr;
}

const Actor* World::find(std::uint64_t id) const {
  for (const Actor& a : actors_) {
    if (a.data.id == id) {
      return &a;
    }
  }
  return nullptr;
}

const Actor* World::player(int slot) const {
  for (const Actor& a : actors_) {
    if (a.data.player && a.data.player->slot == slot) {
      return &a;
    }
  }
  return nullptr;
}

int World::player_count() const {
  int n = 0;
  for (const Actor& a : actors_) {
    if (a.data.player) {
      ++n;
    }
  }
  return n;
}

void World::set_view_size(float width, float height) {
  view_w_ = std::max(1.0f, width);
  view_h_ = std::max(1.0f, height);
}

// --- Camera -------------------------------------------------------------------

// The camera Play looks through: a forced one (camera.set_active), else
// the active Camera2D with the highest priority (ties: scene order).
std::uint64_t World::pick_camera() const {
  if (forced_cam_ != 0) {
    const Actor* f = find(forced_cam_);
    if (f && f->data.camera) return forced_cam_;
  }
  const Actor* best = nullptr;
  for (const Actor& a : actors_) {
    if (a.dead || !a.data.camera || !a.data.camera->active) continue;
    if (!best || a.data.camera->priority > best->data.camera->priority) best = &a;
  }
  return best ? best->data.id : 0;
}

void World::setup_camera() {
  cam_ = CameraRig{};
  if (const Actor* a = find(pick_camera())) {
    cam_.entity = a->data.id;
    cam_.data = *a->data.camera;
    cam_.target = a->data.camera->target;
    cam_.x = a->data.center_x();
    cam_.y = a->data.center_y();
  }
  if (cam_.entity == 0) {
    // No Camera2D in the scene: an implicit, rigid camera on player 0.
    cam_.data.smoothing = 0.0f;
    if (const Actor* p = player(0)) {
      cam_.target = p->data.id;
    } else if (!actors_.empty()) {
      // Nobody to follow: frame the middle of everything.
      float x0 = actors_.front().data.x;
      float y0 = actors_.front().data.y;
      float x1 = x0 + actors_.front().data.w;
      float y1 = y0 + actors_.front().data.h;
      for (const Actor& a : actors_) {
        x0 = std::min(x0, a.data.x);
        y0 = std::min(y0, a.data.y);
        x1 = std::max(x1, a.data.x + a.data.w);
        y1 = std::max(y1, a.data.y + a.data.h);
      }
      cam_.x = (x0 + x1) * 0.5f;
      cam_.y = (y0 + y1) * 0.5f;
    }
  }
  // Start on the target, not a smoothing-length pan away from it.
  float tx = 0.0f;
  float ty = 0.0f;
  if (target_center(&tx, &ty)) {
    cam_.x = tx;
    cam_.y = ty;
  }
  clamp_camera(&cam_.x, &cam_.y);
  cam_.prev_x = cam_.x;
  cam_.prev_y = cam_.y;
  if (Actor* ce = find(cam_.entity)) {
    ce->data.x = cam_.x - ce->data.w * 0.5f;
    ce->data.y = cam_.y - ce->data.h * 0.5f;
    ce->prev_x = ce->data.x;
    ce->prev_y = ce->data.y;
  }
}

bool World::set_active_camera(std::uint64_t id) {
  if (id != 0) {
    const Actor* a = find(id);
    if (!a || !a->data.camera) return false;
  }
  forced_cam_ = id;
  if (pick_camera() != cam_.entity) setup_camera();
  return true;
}

bool World::set_camera_priority(std::uint64_t id, int priority) {
  Actor* a = find(id);
  if (!a || !a->data.camera) return false;
  a->data.camera->priority = priority;
  return true;
}

bool World::set_camera_zoom(std::uint64_t id, float zoom) {
  Actor* a = find(id);
  if (!a || !a->data.camera) return false;
  a->data.camera->zoom = std::clamp(zoom, 0.05f, 50.0f);
  if (id == cam_.entity) cam_.data.zoom = a->data.camera->zoom;
  return true;
}

std::size_t World::camera_count() const {
  std::size_t n = 0;
  for (const Actor& a : actors_) n += (!a.dead && a.data.camera) ? 1u : 0u;
  return n;
}

bool World::target_center(float* x, float* y) const {
  if (cam_.target == 0 || cam_.target == cam_.entity) {
    return false;
  }
  const Actor* t = find(cam_.target);
  if (!t) {
    return false;
  }
  *x = t->data.center_x();
  *y = t->data.center_y();
  return true;
}

void World::clamp_camera(float* x, float* y) const {
  if (!cam_.data.use_bounds) {
    return;
  }
  const Camera2DData& c = cam_.data;
  const float half_w = view_w_ * 0.5f / c.zoom;
  const float half_h = view_h_ * 0.5f / c.zoom;
  auto clamp_axis = [](float v, float lo, float size, float half) {
    if (size <= half * 2.0f) {
      return lo + size * 0.5f;  // view wider than the bounds: centre it
    }
    return std::clamp(v, lo + half, lo + size - half);
  };
  *x = clamp_axis(*x, c.bounds_x, c.bounds_w, half_w);
  *y = clamp_axis(*y, c.bounds_y, c.bounds_h, half_h);
}

void World::step_camera() {
  // Another camera took over (priority / active / scripts): cut to it.
  if (pick_camera() != cam_.entity) {
    setup_camera();
    return;
  }
  if (const Actor* ce = find(cam_.entity); ce && ce->data.camera) {
    // Live settings (scripts, inspector) except the follow target.
    const std::uint64_t keep = cam_.target;
    cam_.data = *ce->data.camera;
    cam_.target = keep;
  }
  cam_.prev_x = cam_.x;
  cam_.prev_y = cam_.y;
  float tx = 0.0f;
  float ty = 0.0f;
  if (target_center(&tx, &ty)) {
    // Dead zone: the target roams a centred box without moving the view.
    const float hw = cam_.data.dead_zone_w * 0.5f;
    const float hh = cam_.data.dead_zone_h * 0.5f;
    if (hw > 0.0f) tx = tx > cam_.x + hw ? tx - hw : tx < cam_.x - hw ? tx + hw : cam_.x;
    if (hh > 0.0f) ty = ty > cam_.y + hh ? ty - hh : ty < cam_.y - hh ? ty + hh : cam_.y;
    if (cam_.data.smoothing <= 0.0f) {
      cam_.x = tx;
      cam_.y = ty;
    } else {
      // Exponential catch-up: frame-rate independent, ~63% of the gap per
      // `smoothing` seconds.
      const float k = 1.0f - static_cast<float>(std::exp(
                                 -kTickSeconds / cam_.data.smoothing));
      cam_.x += (tx - cam_.x) * k;
      cam_.y += (ty - cam_.y) * k;
    }
  }
  clamp_camera(&cam_.x, &cam_.y);
  // Keep the camera entity's rect on the view centre (scripts read it).
  if (Actor* ce = find(cam_.entity)) {
    ce->prev_x = ce->data.x;
    ce->prev_y = ce->data.y;
    ce->data.x = cam_.x - ce->data.w * 0.5f;
    ce->data.y = cam_.y - ce->data.h * 0.5f;
  }
}

CameraView World::camera(float alpha) const {
  alpha = std::clamp(alpha, 0.0f, 1.0f);
  CameraView v;
  v.x = cam_.prev_x + (cam_.x - cam_.prev_x) * alpha;
  v.y = cam_.prev_y + (cam_.y - cam_.prev_y) * alpha;
  v.zoom = cam_.data.zoom;
  v.use_clear = cam_.entity != 0 && cam_.data.use_clear;
  std::copy(cam_.data.clear_color, cam_.data.clear_color + 4, v.clear);
  if (cam_.entity != 0) std::copy(cam_.data.viewport, cam_.data.viewport + 4, v.viewport);
  return v;
}

WorldRect World::view_rect(const CameraView& cam, float width, float height) {
  const float zoom = cam.zoom > 0.0f ? cam.zoom : 1.0f;
  const float hw = width * 0.5f / zoom;
  const float hh = height * 0.5f / zoom;
  return WorldRect{cam.x - hw, cam.y - hh, cam.x + hw, cam.y + hh};
}

// --- Simulation ---------------------------------------------------------------

void World::step_players(const InputFrame& input) {
  for (Actor& a : actors_) {
    a.prev_x = a.data.x;
    a.prev_y = a.data.y;
    a.vx = 0.0f;
    a.vy = 0.0f;
    if (!a.data.player) {
      continue;
    }
    move_rider(a, input.slot(a.data.player->slot));
  }
}

float top_down_aim_degrees(float dx, float dy) {
  // atan2 on screen axes (y down) is clockwise from +x; art faces up.
  float deg = std::atan2(dy, dx) * 57.29577951308232f + 90.0f;
  if (deg < 0.0f) deg += 360.0f;
  if (deg >= 360.0f) deg -= 360.0f;
  return deg;
}

void aim_at_screen(PlayerInput* in, const CameraView& cam, float view_w, float view_h,
                   float sx, float sy) {
  if (!in || view_w <= 0.0f || view_h <= 0.0f) return;
  const WorldRect r = World::view_rect(cam, view_w, view_h);
  in->aim = true;
  in->aim_x = r.x0 + sx / view_w * (r.x1 - r.x0);
  in->aim_y = r.y0 + sy / view_h * (r.y1 - r.y0);
}

bool aim_along_stick(PlayerInput* in, const World& world, int slot, float x, float y) {
  if (!in || x * x + y * y < 0.3f * 0.3f) return false;
  const Actor* p = world.player(slot);
  if (!p) return false;
  const float len = std::sqrt(x * x + y * y);
  in->aim = true;
  in->aim_x = p->data.x + p->data.w * 0.5f + x / len * 128.0f;
  in->aim_y = p->data.y + p->data.h * 0.5f + y / len * 128.0f;
  return true;
}

void World::move_rider(Actor& a, const PlayerInput& in) {
  // Movement is a Trading Post plugin (RiderController2D.h), picked by the
  // rider's mode.
  const bool platformer = a.data.player->mode == PlayerControllerData::Mode::Platformer;
  const char* id = platformer ? kPlatformerRiderId : kTopDownRiderId;
  if (const auto ctrl = RiderControllers2D::instance().get(id)) {
    ctrl->move(*this, a, in, static_cast<float>(kTickSeconds));
    return;
  }
  // No movement plugin for this mode: the rider stays put.
  a.in_x = 0.0f;
  a.in_y = 0.0f;
  a.aiming = false;
  const unsigned bit = platformer ? 2u : 1u;
  if ((rider_warned_ & bit) == 0) {
    rider_warned_ |= bit;
    post(LogLevel::Warn, "play",
         a.data.name + std::string(" has a ") + (platformer ? "platformer" : "top-down") +
             " Player but no movement plugin is on; it stays put. Switch on " +
             (platformer ? "Platformer Rider (ts.move.platformer)" : "Top-down Rider (ts.move.topdown)") +
             " in the Trading Post.",
         a.data.id);
  }
}

void World::step(const InputFrame& input) {
  flush_delayed();
  if (actors_.empty()) {
    input_ = input;
    ++tick_;
    return;
  }
  TS_PROFILE_SCOPE("world.step");
  events_.clear();
  input_ = input;
  in_step_ = true;
  // OnStart, then OnFixedUpdate, then the physics step, then OnUpdate / OnLateUpdate.
  if (scripts_) {
    scripts_->start_pending();
    scripts_->fixed_tick(kTickSeconds);
  }
  apply_changes();
  {
    TS_PROFILE_SCOPE("world.physics");
    step_players(input);
    separate_dynamics();
    step_physics2d();
    update_triggers();
    update_collisions();
  }
  for (const TriggerEvent& ev : events_) {
    post(LogLevel::Info, "trigger", describe(ev), ev.trigger);
  }
  {
    TS_PROFILE_SCOPE("world.scripts");
    step_scripts(input);
    if (scripts_) scripts_->late_tick(kTickSeconds);
  }
  {
    TS_PROFILE_SCOPE("world.plugins");
    step_extensions();
  }
  follow_parents();
  apply_follows();
  {
    TS_PROFILE_SCOPE("world.animation");
    step_animation();
  }
  step_camera();
  {
    TS_PROFILE_SCOPE("world.fx");
    step_fx();
  }
  in_step_ = false;
  ++tick_;
  if (profiler::Profiler::get().enabled()) {
    profiler::Profiler& prof = profiler::Profiler::get();
    prof.counter("world.entities", static_cast<double>(actors_.size()));
    if (scripts_) prof.counter("mem.lua_kb", static_cast<double>(scripts_->memory_used()) / 1024.0);
    prof.add("world.ticks", 1);
  }
  // A toast shows through its `until` tick.
  toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                               [&](const Toast& t) { return t.until < tick_; }),
                toasts_.end());
}

void World::follow_parents() {
  // depth[i]: how many parents up to the root (loops count as roots).
  std::vector<std::pair<int, std::size_t>> order;
  for (std::size_t i = 0; i < actors_.size(); ++i) {
    if (actors_[i].data.parent == 0 || actors_[i].dead) continue;
    int depth = 0;
    std::uint64_t at = actors_[i].data.parent;
    while (at != 0 && depth <= static_cast<int>(actors_.size())) {
      const Actor* p = find(at);
      if (!p) break;
      ++depth;
      at = p->data.parent;
    }
    order.emplace_back(depth, i);
  }
  if (order.empty()) return;
  std::stable_sort(order.begin(), order.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [depth, i] : order) {
    Actor& c = actors_[i];
    const Actor* p = find(c.data.parent);
    if (!p || p == &c) continue;
    c.data.x += p->data.x - p->prev_x;
    c.data.y += p->data.y - p->prev_y;
  }
}

// Follow constraints (Entity2D::follow), after parents carried children.
void World::apply_follows() {
  for (Actor& a : actors_) {
    if (a.dead || !a.data.follow || a.data.follow->target == 0) continue;
    const Actor* t = find(a.data.follow->target);
    if (!t || t == &a) continue;
    const FollowData& f = *a.data.follow;
    if (f.position) {
      float ox = f.offset_x, oy = f.offset_y;
      if (f.rotation) {
        const float r = t->data.rotation * 3.14159265f / 180.0f;
        const float c = std::cos(r), sn = std::sin(r);
        const float rx = ox * c - oy * sn;
        oy = ox * sn + oy * c;
        ox = rx;
      }
      a.data.x = t->data.center_x() + ox - a.data.w * 0.5f;
      a.data.y = t->data.center_y() + oy - a.data.h * 0.5f;
    }
    if (f.rotation) a.data.rotation = t->data.rotation + f.rotation_offset;
  }
}

// --- Draw list ------------------------------------------------------------------

namespace {

void push_solid(std::vector<DrawQuad>* out, float x0, float y0, float x1,
                float y1, const float rgba[4], std::uint64_t entity) {
  DrawQuad q;
  q.x0 = x0;
  q.y0 = y0;
  q.x1 = x1;
  q.y1 = y1;
  for (int i = 0; i < 4; ++i) {
    q.rgba[i] = rgba[i];
  }
  q.entity = entity;
  out->push_back(q);
}

}  // namespace

void World::build_draw_list(const WorldRect& view, float alpha,
                            const ImageSizeFn& image_size,
                            std::vector<DrawQuad>* out) const {
  out->clear();
  alpha = std::clamp(alpha, 0.0f, 1.0f);
  auto size_of = [&](const std::string& rel, int* w, int* h) {
    return !rel.empty() && image_size && image_size(rel, w, h) && *w > 0 &&
           *h > 0;
  };
  auto layers = [&](bool over, bool hud = false) {
    if (!ext_) return;
    for (const WorldExtensions::DrawLayer& l : ext_->layers) {
      if (l.hud != hud || (!hud && l.over != over) || !l.draw) continue;
      try {
        l.draw(*this, view, alpha, out);
      } catch (...) {
        // A broken layer draws nothing; the system side reports errors.
      }
    }
  };
  layers(false);
  for (std::size_t idx : draw_order_) {
    const Actor& a = actors_[idx];
    const Entity2D& e = a.data;
    if (e.camera || e.spawn || a.hidden || a.dead || e.w <= 0.0f ||
        e.h <= 0.0f) {
      continue;
    }
    const float x = a.prev_x + (e.x - a.prev_x) * alpha;
    const float y = a.prev_y + (e.y - a.prev_y) * alpha;
    if (x > view.x1 || y > view.y1 || x + e.w < view.x0 || y + e.h < view.y0) {
      continue;
    }
    if (e.tilemap) {
      const TileMapData& tm = *e.tilemap;
      const float ts = static_cast<float>(tm.tile_size);
      int img_w = 0;
      int img_h = 0;
      const bool has_set = size_of(tm.tileset, &img_w, &img_h) &&
                           tileset_tile_count(tm.tile_size, img_w, img_h) > 0;
      const int c0 = std::max(0, static_cast<int>(std::floor((view.x0 - x) / ts)));
      const int r0 = std::max(0, static_cast<int>(std::floor((view.y0 - y) / ts)));
      const int c1 = std::min(tm.cols - 1, static_cast<int>(std::floor((view.x1 - x) / ts)));
      const int r1 = std::min(tm.rows - 1, static_cast<int>(std::floor((view.y1 - y) / ts)));
      for (int r = r0; r <= r1; ++r) {
        for (int c = c0; c <= c1; ++c) {
          const int t = tm.at(c, r);
          if (t <= 0) {
            continue;
          }
          const float qx0 = x + static_cast<float>(c) * ts;
          const float qy0 = y + static_cast<float>(r) * ts;
          DrawQuad q;
          q.x0 = qx0;
          q.y0 = qy0;
          q.x1 = qx0 + ts;
          q.y1 = qy0 + ts;
          q.entity = e.id;
          if (has_set && tileset_uv(t, tm.tile_size, img_w, img_h, &q.u0,
                                    &q.v0, &q.u1, &q.v1)) {
            q.image = &tm.tileset;
            q.rgba[3] = e.color[3];
            out->push_back(q);
            continue;
          }
          // Built-in frontier tile with the same hand-cut bevel the editor
          // draws: lit top edge, shaded bottom lip.
          const BuiltinTile& bt = builtin_tile(t);
          const float base[4] = {bt.rgba[0], bt.rgba[1], bt.rgba[2],
                                 bt.rgba[3] * e.color[3]};
          push_solid(out, q.x0, q.y0, q.x1, q.y1, base, e.id);
          const float lip = std::max(1.0f, ts * 0.12f);
          const float dark[4] = {base[0] * 0.72f, base[1] * 0.72f,
                                 base[2] * 0.72f, base[3]};
          push_solid(out, q.x0, q.y1 - lip, q.x1, q.y1, dark, e.id);
          const float lit[4] = {std::min(1.0f, base[0] * 1.2f),
                                std::min(1.0f, base[1] * 1.2f),
                                std::min(1.0f, base[2] * 1.2f), base[3]};
          push_solid(out, q.x0, q.y0, q.x1, q.y0 + std::max(1.0f, ts / 32.0f),
                     lit, e.id);
        }
      }
      continue;
    }
    DrawQuad q;
    q.x0 = x;
    q.y0 = y;
    q.x1 = x + e.w;
    q.y1 = y + e.h;
    for (int i = 0; i < 4; ++i) {
      q.rgba[i] = e.color[i];
    }
    q.entity = e.id;
    q.rotation = e.rotation + (a.aiming ? a.aim_deg : 0.0f);
    const AnimLibrary::Entry* anim = anim_set(a);
    if (anim && anim->ok) {
      const AnimClip* clip = anim->set.find(a.anim.clip);
      int img_w = 0;
      int img_h = 0;
      if (clip && size_of(anim->image, &img_w, &img_h)) {
        const AnimRect r = anim->set.frame_rect(*clip, a.anim.frame);
        q.image = &anim->image;
        q.u0 = static_cast<float>(r.x) / static_cast<float>(img_w);
        q.v0 = static_cast<float>(r.y) / static_cast<float>(img_h);
        q.u1 = static_cast<float>(r.x + r.w) / static_cast<float>(img_w);
        q.v1 = static_cast<float>(r.y + r.h) / static_cast<float>(img_h);
        const bool sprite_flip_x = e.sprite && e.sprite->flip_x;
        if (sprite_flip_x != a.anim.flip_x) std::swap(q.u0, q.u1);
        if (e.sprite && e.sprite->flip_y) std::swap(q.v0, q.v1);
      } else {
        q.missing = true;
      }
    } else if (e.sprite) {
      const SpriteData& sp = *e.sprite;
      int img_w = 0;
      int img_h = 0;
      if (size_of(sp.path, &img_w, &img_h)) {
        q.image = &sp.path;
        if (sp.use_src_rect) {
          const int sw = sp.src_w > 0 ? sp.src_w : img_w - sp.src_x;
          const int sh = sp.src_h > 0 ? sp.src_h : img_h - sp.src_y;
          q.u0 = static_cast<float>(sp.src_x) / static_cast<float>(img_w);
          q.v0 = static_cast<float>(sp.src_y) / static_cast<float>(img_h);
          q.u1 = static_cast<float>(sp.src_x + sw) / static_cast<float>(img_w);
          q.v1 = static_cast<float>(sp.src_y + sh) / static_cast<float>(img_h);
        }
        if (sp.flip_x) std::swap(q.u0, q.u1);
        if (sp.flip_y) std::swap(q.v0, q.v1);
      } else {
        q.missing = true;
      }
    } else if (anim) {
      q.missing = true;  // broken / missing .anim.json and no sprite
    }
    out->push_back(q);
  }
  build_particles(false, out);
  layers(true);
  build_lightmap(view, out);
  build_particles(true, out);
  layers(true, true);
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
