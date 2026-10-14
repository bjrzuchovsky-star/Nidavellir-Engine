#include "runtime/World.h"

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
  if (entities.empty()) {
    if (error_out) *error_out = "The scene is empty. Nothing to ride.";
    return false;
  }
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
  scene_json::SceneDoc doc;
  if (!scene_json::load_file(scene_json::scene_path_for_project(project_dir),
                             &doc, error_out)) {
    return false;
  }
  return build(doc.entities, doc.tile_solidity, project_dir, error_out);
}

void World::clear() {
  scripts_.reset();
  ext_dead_.assign(ext_ ? ext_->systems.size() : 0, false);
  authored_.clear();
  logs_.clear();
  toasts_.clear();
  sounds_.clear();
  next_id_ = 1;
  prev_buttons_ = {};
  in_step_ = false;
  dirty_ = false;
  actors_.clear();
  draw_order_.clear();
  cam_ = CameraRig{};
  tick_ = 0;
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

void World::setup_camera() {
  cam_ = CameraRig{};
  for (const Actor& a : actors_) {
    if (a.data.camera) {
      cam_.entity = a.data.id;
      cam_.data = *a.data.camera;
      cam_.target = a.data.camera->target;
      cam_.x = a.data.center_x();
      cam_.y = a.data.center_y();
      break;
    }
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
  cam_.prev_x = cam_.x;
  cam_.prev_y = cam_.y;
  float tx = 0.0f;
  float ty = 0.0f;
  if (target_center(&tx, &ty)) {
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

void World::move_rider(Actor& a, const PlayerInput& in) {
  const float dt = static_cast<float>(kTickSeconds);
  float mx = std::clamp(in.move_x, -1.0f, 1.0f);
  float my = std::clamp(in.move_y, -1.0f, 1.0f);
  // Diagonals are no faster than straight lines.
  const float len = std::sqrt(mx * mx + my * my);
  if (len > 1.0f) {
    mx /= len;
    my /= len;
  }
  a.in_x = mx;
  a.in_y = my;
  const float x0 = a.data.x;
  const float y0 = a.data.y;
  move_body(a, mx * a.data.player->speed * dt, my * a.data.player->speed * dt);
  // Report what actually happened (0 when stopped flush on a wall).
  a.vx = (a.data.x - x0) / dt;
  a.vy = (a.data.y - y0) / dt;
  if (mx > 0.0f) {
    a.facing = 1;
  } else if (mx < 0.0f) {
    a.facing = -1;
  }
}

void World::step(const InputFrame& input) {
  if (actors_.empty()) {
    return;
  }
  events_.clear();
  input_ = input;
  in_step_ = true;
  // Scripts that joined since the last tick start before anybody moves.
  if (scripts_) {
    scripts_->start_pending();
  }
  apply_changes();
  step_players(input);
  separate_dynamics();
  update_triggers();
  for (const TriggerEvent& ev : events_) {
    post(LogLevel::Info, "trigger", describe(ev), ev.trigger);
  }
  step_scripts(input);
  step_extensions();
  follow_parents();
  step_animation();
  step_camera();
  in_step_ = false;
  ++tick_;
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
  auto layers = [&](bool over) {
    if (!ext_) return;
    for (const WorldExtensions::DrawLayer& l : ext_->layers) {
      if (l.over != over || !l.draw) continue;
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
    q.rotation = e.rotation;
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
  layers(true);
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
