#include "editor/workspace/Workspace2D.h"

#include "scene/SampleRider.h"
#include "scene/SampleScripts.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

constexpr float kMinZoom = 0.15f;
constexpr float kMaxZoom = 8.0f;

}  // namespace

Workspace2D::Workspace2D() {
  reset_defaults();
}

void Workspace2D::reset_defaults() {
  entities_.clear();
  tile_solidity_ = TileSolidity{};
  selected_id_.reset();
  selection_.clear();
  move_starts_.clear();
  move_active_ = false;
  next_id_ = 1;
  pan_x_ = 0.0f;
  pan_y_ = 0.0f;
  zoom_ = 1.0f;
  show_grid_ = true;

  Entity2D camera;
  camera.id = alloc_id();
  camera.name = "Camera2D";
  camera.x = 0.0f;
  camera.y = 0.0f;
  camera.w = 48.0f;
  camera.h = 32.0f;
  camera.color[0] = 0.95f;
  camera.color[1] = 0.75f;
  camera.color[2] = 0.25f;
  camera.color[3] = 1.0f;
  camera.layer = 10;
  entities_.push_back(camera);

  Entity2D player;
  player.id = alloc_id();
  player.name = "Player";
  player.x = 64.0f;
  player.y = 64.0f;
  player.w = 32.0f;
  player.h = 48.0f;
  // Untinted: the rider sheet brings its own colours.
  player.color[0] = 1.0f;
  player.color[1] = 1.0f;
  player.color[2] = 1.0f;
  player.color[3] = 1.0f;
  player.layer = 5;
  // Rides on player slot 0 (WASD / arrows / first gamepad) in Play mode.
  player.player = PlayerControllerData{};
  // Bumps into solid tiles and static colliders (whole rect, dynamic).
  player.collider = default_collider(player);
  // The sample cowboy sheet (written into assets/ when a scene is seeded):
  // the sprite shows the first idle frame, the animator rides the rest.
  player.sprite = SpriteData{};
  player.sprite->path = sample_rider::kImage;
  player.sprite->use_src_rect = true;
  player.sprite->src_w = sample_rider::kFrameW;
  player.sprite->src_h = sample_rider::kFrameH;
  player.animator = AnimatorData{};
  player.animator->set = sample_rider::kSet;
  entities_.push_back(player);
  // The seeded camera trails the rider.
  Camera2DData follow;
  follow.target = player.id;
  entities_.front().camera = follow;

  // A real 8x2 TileMap (32 px cells, 256x64 like the old placeholder):
  // a grass top row over dirt so the seed scene shows painted tiles.
  Entity2D tilemap;
  tilemap.id = alloc_id();
  tilemap.name = "TileMap";
  tilemap.x = 0.0f;
  tilemap.y = 160.0f;
  tilemap.color[0] = 0.45f;
  tilemap.color[1] = 0.45f;
  tilemap.color[2] = 0.55f;
  tilemap.color[3] = 1.0f;
  tilemap.layer = 0;
  tilemap.tilemap = TileMapData(8, 2, 32);
  for (int c = 0; c < 8; ++c) {
    tilemap.tilemap->set(c, 0, 4);  // Grass
    tilemap.tilemap->set(c, 1, 1);  // Dirt
  }
  sync_tilemap_extent(tilemap);
  entities_.push_back(tilemap);

  // Scripted samples (scripts/gate.lua, scripts/gold.lua; written next to
  // the rider sheet): ride left onto the nugget, then press E / Space /
  // pad A at the gate. Both sit left of the rider, off the usual trail.
  Entity2D gate;
  gate.id = alloc_id();
  gate.name = sample_scripts::kGateName;
  gate.x = 0.0f;
  gate.y = 48.0f;
  gate.w = 16.0f;
  gate.h = 80.0f;
  gate.color[0] = 0.55f;  // weathered pine
  gate.color[1] = 0.36f;
  gate.color[2] = 0.20f;
  gate.color[3] = 1.0f;
  gate.layer = 4;
  gate.collider = default_collider(gate);  // solid until it swings open
  gate.script = ScriptData{};
  gate.script->path = sample_scripts::kGate;
  entities_.push_back(gate);

  Entity2D gold;
  gold.id = alloc_id();
  gold.name = sample_scripts::kGoldName;
  gold.x = 28.0f;
  gold.y = 80.0f;
  gold.w = 16.0f;
  gold.h = 16.0f;
  gold.color[0] = 0.96f;
  gold.color[1] = 0.78f;
  gold.color[2] = 0.26f;
  gold.color[3] = 1.0f;
  gold.layer = 4;
  gold.collider = default_collider(gold);
  gold.collider->trigger = true;
  gold.script = ScriptData{};
  gold.script->path = sample_scripts::kGold;
  entities_.push_back(gold);

  select(player.id);
}


void Workspace2D::replace_scene(std::vector<Entity2D> entities,
                                std::optional<std::uint64_t> selected_id,
                                float pan_x, float pan_y, float zoom,
                                bool show_grid) {
  entities_ = std::move(entities);
  selected_id_.reset();
  selection_.clear();
  move_starts_.clear();
  move_active_ = false;
  pan_x_ = pan_x;
  pan_y_ = pan_y;
  set_zoom(zoom);
  show_grid_ = show_grid;
  sync_next_id_from_entities();
  if (selected_id && find(*selected_id)) {
    select(*selected_id);
  } else if (!entities_.empty()) {
    select(entities_.front().id);
  }
}

void Workspace2D::set_tile_solidity(TileSolidity solidity) {
  solidity.normalize();
  tile_solidity_ = std::move(solidity);
}

bool Workspace2D::set_tile_solid(const std::string& tileset, int tile_id,
                                 bool solid) {
  return tile_solidity_.set_solid(tileset, tile_id, solid);
}

bool Workspace2D::set_collider(std::uint64_t id,
                               std::optional<ColliderData> collider) {
  Entity2D* e = find(id);
  if (!e) {
    return false;
  }
  const std::optional<ColliderData> before = e->collider;
  e->collider = std::move(collider);
  normalize_components(*e);
  return e->collider != before;
}

void Workspace2D::sync_next_id_from_entities() {
  std::uint64_t max_id = 0;
  for (const auto& e : entities_) {
    if (e.id > max_id) {
      max_id = e.id;
    }
  }
  next_id_ = max_id + 1;
  if (next_id_ == 0) {
    next_id_ = 1;
  }
}

bool Workspace2D::is_selected(std::uint64_t id) const {
  return std::find(selection_.begin(), selection_.end(), id) !=
         selection_.end();
}

void Workspace2D::select(std::optional<std::uint64_t> id) {
  if (!id) {
    clear_selection();
    return;
  }
  if (find(*id)) {
    selected_id_ = id;
    selection_.assign(1, *id);
  }
}

void Workspace2D::add_to_selection(std::uint64_t id) {
  if (!find(id)) {
    return;
  }
  if (!is_selected(id)) {
    selection_.push_back(id);
  }
  selected_id_ = id;
}

void Workspace2D::toggle_selection(std::uint64_t id) {
  if (!find(id)) {
    return;
  }
  const auto it = std::find(selection_.begin(), selection_.end(), id);
  if (it != selection_.end()) {
    selection_.erase(it);
    if (selected_id_ && *selected_id_ == id) {
      if (selection_.empty()) {
        selected_id_.reset();
      } else {
        selected_id_ = selection_.back();
      }
    }
    return;
  }
  selection_.push_back(id);
  selected_id_ = id;
}

void Workspace2D::set_selection(const std::vector<std::uint64_t>& ids,
                                std::optional<std::uint64_t> primary) {
  selection_.clear();
  for (std::uint64_t id : ids) {
    if (find(id) && !is_selected(id)) {
      selection_.push_back(id);
    }
  }
  if (primary && is_selected(*primary)) {
    selected_id_ = primary;
  } else if (!selection_.empty()) {
    selected_id_ = selection_.back();
  } else {
    selected_id_.reset();
  }
}

void Workspace2D::select_all() {
  std::vector<std::uint64_t> ids;
  ids.reserve(entities_.size());
  for (const auto& e : entities_) {
    ids.push_back(e.id);
  }
  set_selection(ids, selected_id_);
}

void Workspace2D::clear_selection() {
  selected_id_.reset();
  selection_.clear();
}

std::size_t Workspace2D::select_in_rect(float x0, float y0, float x1, float y1,
                                        bool additive) {
  const float minx = std::min(x0, x1);
  const float maxx = std::max(x0, x1);
  const float miny = std::min(y0, y1);
  const float maxy = std::max(y0, y1);
  std::vector<std::uint64_t> ids;
  if (additive) {
    ids = selection_;
  }
  std::size_t hits = 0;
  std::optional<std::uint64_t> primary = additive ? selected_id_ : std::nullopt;
  for (std::size_t idx : sorted_draw_order()) {
    const Entity2D& e = entities_[idx];
    if (effectively_hidden(e) || effectively_locked(e)) {
      continue;
    }
    const bool overlap = e.x <= maxx && e.x + e.w >= minx && e.y <= maxy &&
                         e.y + e.h >= miny;
    if (!overlap) {
      continue;
    }
    ++hits;
    if (std::find(ids.begin(), ids.end(), e.id) == ids.end()) {
      ids.push_back(e.id);
    }
    if (!primary) {
      primary = e.id;  // lowest layer hit becomes primary unless additive
    }
  }
  set_selection(ids, primary);
  return hits;
}

std::optional<std::uint64_t> Workspace2D::pick(float wx, float wy) const {
  std::optional<std::uint64_t> hit;
  for (std::size_t idx : sorted_draw_order()) {
    const Entity2D& e = entities_[idx];
    if (effectively_hidden(e) || effectively_locked(e)) {
      continue;
    }
    // Turned entities: test in their own frame.
    float px = wx;
    float py = wy;
    if (e.rotation != 0.0f) {
      const float a = -e.rotation * 3.14159265f / 180.0f;
      const float dx = wx - e.center_x();
      const float dy = wy - e.center_y();
      px = e.center_x() + dx * std::cos(a) - dy * std::sin(a);
      py = e.center_y() + dx * std::sin(a) + dy * std::cos(a);
    }
    if (px >= e.x && py >= e.y && px <= e.x + e.w && py <= e.y + e.h) {
      hit = e.id;  // draw order ascending -> last hit is topmost
    }
  }
  return hit;
}

void Workspace2D::prune_selection() {
  selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                  [this](std::uint64_t id) {
                                    return find(id) == nullptr;
                                  }),
                   selection_.end());
  if (selected_id_ && !is_selected(*selected_id_)) {
    selected_id_.reset();
  }
  if (!selected_id_ && !selection_.empty()) {
    selected_id_ = selection_.back();
  }
}

Entity2D* Workspace2D::find(std::uint64_t id) {
  for (auto& e : entities_) {
    if (e.id == id) {
      return &e;
    }
  }
  return nullptr;
}

const Entity2D* Workspace2D::find(std::uint64_t id) const {
  for (const auto& e : entities_) {
    if (e.id == id) {
      return &e;
    }
  }
  return nullptr;
}

Entity2D* Workspace2D::selected() {
  if (!selected_id_) {
    return nullptr;
  }
  return find(*selected_id_);
}

const Entity2D* Workspace2D::selected() const {
  if (!selected_id_) {
    return nullptr;
  }
  return find(*selected_id_);
}

std::uint64_t Workspace2D::create_entity(std::string name) {
  if (name.empty()) {
    name = "Entity";
  }
  Entity2D e;
  e.id = alloc_id();
  e.name = unique_name(entities_, std::move(name));
  e.x = 32.0f + static_cast<float>(entities_.size() % 5) * 24.0f;
  e.y = 32.0f + static_cast<float>(entities_.size() % 4) * 24.0f;
  e.w = 64.0f;
  e.h = 64.0f;
  e.layer = static_cast<int>(entities_.size());
  entities_.push_back(e);
  select(e.id);
  return e.id;
}

bool Workspace2D::rename_entity(std::uint64_t id, std::string name) {
  Entity2D* e = find(id);
  if (!e || name.empty()) {
    return false;
  }
  e->name = std::move(name);
  return true;
}

bool Workspace2D::delete_entity(std::uint64_t id) {
  const auto it =
      std::find_if(entities_.begin(), entities_.end(),
                   [id](const Entity2D& e) { return e.id == id; });
  if (it == entities_.end()) {
    return false;
  }
  // Its children move up to its parent.
  const std::uint64_t up = it->parent;
  entities_.erase(it);
  for (Entity2D& e : entities_) {
    if (e.parent == id) e.parent = up;
  }
  const bool was_primary = selected_id_ && *selected_id_ == id;
  prune_selection();
  if (was_primary && selection_.empty() && !entities_.empty()) {
    select(entities_.front().id);
  }
  return true;
}

std::vector<std::uint64_t> Workspace2D::duplicate_selection(bool shift) {
  std::vector<std::uint64_t> new_ids;
  if (selection_.empty()) {
    return new_ids;
  }
  const float offset = !shift ? 0.0f : snap_enabled_ ? grid_size_ : 16.0f;
  // Roots with their subtrees, copied first: push_back below may
  // reallocate entities_.
  std::vector<Entity2D> sources;
  std::vector<std::uint64_t> roots = selection_roots();
  for (std::uint64_t root : roots) {
    if (const Entity2D* e = find(root)) sources.push_back(*e);
    for (std::uint64_t d : descendants_of(root)) {
      if (const Entity2D* e = find(d)) sources.push_back(*e);
    }
  }
  std::vector<std::pair<std::uint64_t, std::uint64_t>> remap;  // old -> new
  for (const Entity2D& src : sources) remap.emplace_back(src.id, alloc_id());
  auto mapped = [&](std::uint64_t old) -> std::uint64_t {
    for (const auto& [a, b] : remap) {
      if (a == old) return b;
    }
    return 0;
  };
  std::optional<std::uint64_t> new_primary;
  for (const Entity2D& src : sources) {
    const bool is_root = std::find(roots.begin(), roots.end(), src.id) != roots.end();
    Entity2D copy = src;
    copy.id = mapped(src.id);
    copy.name = unique_name(entities_, strip_copy_suffix(src.name));
    copy.x = src.x + offset;
    copy.y = src.y + offset;
    if (snap_enabled_ && is_root && shift) {
      copy.x = snap_value(copy.x);
      copy.y = snap_value(copy.y);
    }
    if (const std::uint64_t p = mapped(src.parent)) copy.parent = p;
    copy.locked = is_root ? false : src.locked;
    entities_.push_back(copy);
    if (is_root) new_ids.push_back(copy.id);
    if (selected_id_ && *selected_id_ == src.id) {
      new_primary = copy.id;
    }
  }
  set_selection(new_ids, new_primary);
  return new_ids;
}

std::size_t Workspace2D::delete_selection() {
  if (selection_.empty()) {
    return 0;
  }
  // The selection and every descendant.
  std::vector<std::uint64_t> doomed = selection_;
  for (std::uint64_t id : selection_) {
    for (std::uint64_t d : descendants_of(id)) doomed.push_back(d);
  }
  const std::size_t before = entities_.size();
  entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
                                 [&doomed](const Entity2D& e) {
                                   return std::find(doomed.begin(),
                                                    doomed.end(),
                                                    e.id) != doomed.end();
                                 }),
                  entities_.end());
  clear_selection();
  move_starts_.clear();
  move_active_ = false;
  return before - entities_.size();
}

float Workspace2D::snap_step(float v, int dir, float step) const {
  if (dir == 0) {
    return v;
  }
  const float g = grid_size_;
  const float on = std::round(v / g) * g;
  if (std::abs(on - v) < 1e-3f) {
    return on + static_cast<float>(dir) * step;
  }
  // Off-grid: first land on the next grid line in the nudge direction.
  const float next = dir > 0 ? std::ceil(v / g) * g : std::floor(v / g) * g;
  return next + static_cast<float>(dir) * (step - g);
}

bool Workspace2D::nudge_selection(int dir_x, int dir_y, bool large) {
  if (selection_.empty() || (dir_x == 0 && dir_y == 0)) {
    return false;
  }
  const float step = snap_enabled_ ? grid_size_ * (large ? 4.0f : 1.0f)
                                   : (large ? 10.0f : 1.0f);
  bool moved = false;
  for (std::uint64_t id : selection_roots()) {
    const Entity2D* e = find(id);
    if (!e || effectively_locked(*e)) {
      continue;
    }
    float nx = e->x + static_cast<float>(dir_x) * step;
    float ny = e->y + static_cast<float>(dir_y) * step;
    if (snap_enabled_) {
      nx = snap_step(e->x, dir_x, step);
      ny = snap_step(e->y, dir_y, step);
    }
    if (nx != e->x || ny != e->y) {
      move_subtree(id, nx - e->x, ny - e->y);
      moved = true;
    }
  }
  return moved;
}

bool Workspace2D::snap_selection_to_grid() {
  bool changed = false;
  for (std::uint64_t id : selection_roots()) {
    if (const Entity2D* e = find(id)) {
      if (effectively_locked(*e)) continue;
      const float nx = snap_always(e->x);
      const float ny = snap_always(e->y);
      if (nx != e->x || ny != e->y) {
        move_subtree(id, nx - e->x, ny - e->y);
        changed = true;
      }
    }
  }
  return changed;
}

void Workspace2D::begin_move() {
  move_starts_.clear();
  move_roots_.clear();
  guides_.clear();
  for (std::uint64_t id : moving_set()) {
    if (const Entity2D* e = find(id)) {
      move_starts_.push_back({e->id, e->x, e->y});
    }
  }
  for (std::uint64_t id : selection_roots()) {
    const Entity2D* e = find(id);
    if (!e || effectively_locked(*e)) continue;
    if (selected_id_ && *selected_id_ == id) {
      move_roots_.insert(move_roots_.begin(), id);
    } else {
      move_roots_.push_back(id);
    }
  }
  move_active_ = !move_starts_.empty();
  move_changed_ = false;
}

void Workspace2D::update_move(float dx, float dy) {
  if (!move_active_) {
    return;
  }
  auto start_of = [&](std::uint64_t id) -> const MoveStart* {
    for (const MoveStart& s : move_starts_) {
      if (s.id == id) return &s;
    }
    return nullptr;
  };
  if (snap_enabled_) {
    // Snap the primary (or first) root; everyone else keeps formation.
    const MoveStart* anchor = move_roots_.empty() ? &move_starts_.front()
                                                  : start_of(move_roots_.front());
    if (!anchor) anchor = &move_starts_.front();
    dx = snap_value(anchor->x + dx) - anchor->x;
    dy = snap_value(anchor->y + dy) - anchor->y;
  }
  guides_.clear();
  if (object_snap_ && !move_roots_.empty()) {
    // Bounds of the moving roots at the proposed offset.
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (std::uint64_t id : move_roots_) {
      const MoveStart* st = start_of(id);
      const Entity2D* e = find(id);
      if (!st || !e) continue;
      x0 = std::min(x0, st->x + dx);
      y0 = std::min(y0, st->y + dy);
      x1 = std::max(x1, st->x + dx + e->w);
      y1 = std::max(y1, st->y + dy + e->h);
    }
    if (x0 <= x1) {
      std::vector<std::uint64_t> moving;
      for (const MoveStart& s : move_starts_) moving.push_back(s.id);
      float sx = 0.0f, sy = 0.0f;
      snap_box_to_objects(x0, y0, x1, y1, moving, &sx, &sy, &guides_);
      dx += sx;
      dy += sy;
    }
  }
  for (const MoveStart& s : move_starts_) {
    if (Entity2D* e = find(s.id)) {
      const float nx = s.x + dx;
      const float ny = s.y + dy;
      if (nx != e->x || ny != e->y) {
        e->x = nx;
        e->y = ny;
      }
      if (e->x != s.x || e->y != s.y) {
        move_changed_ = true;
      }
    }
  }
}

bool Workspace2D::end_move() {
  bool changed = false;
  if (move_active_) {
    for (const MoveStart& s : move_starts_) {
      if (const Entity2D* e = find(s.id)) {
        if (e->x != s.x || e->y != s.y) {
          changed = true;
        }
      }
    }
  }
  move_starts_.clear();
  move_roots_.clear();
  guides_.clear();
  move_active_ = false;
  move_changed_ = false;
  return changed;
}

void Workspace2D::set_pan(float x, float y) {
  pan_x_ = x;
  pan_y_ = y;
}

void Workspace2D::add_pan(float dx, float dy) {
  pan_x_ += dx;
  pan_y_ += dy;
}

void Workspace2D::cancel_move() {
  for (const MoveStart& s : move_starts_) {
    if (Entity2D* e = find(s.id)) {
      e->x = s.x;
      e->y = s.y;
    }
  }
  move_starts_.clear();
  move_roots_.clear();
  guides_.clear();
  move_active_ = false;
  move_changed_ = false;
}

void Workspace2D::set_grid_size(float g) {
  if (!(g == g)) {  // NaN guard
    return;
  }
  grid_size_ = std::clamp(g, kMinGrid, kMaxGrid);
}

float Workspace2D::snap_always(float v) const {
  if (grid_size_ <= 0.0f) {
    return v;
  }
  return std::round(v / grid_size_) * grid_size_;
}

float Workspace2D::snap_value(float v) const {
  return snap_enabled_ ? snap_always(v) : v;
}

float Workspace2D::snap_extent(float v) const {
  if (!snap_enabled_) {
    return v;
  }
  return std::max(grid_size_, snap_always(v));
}

void Workspace2D::set_zoom(float z) {
  zoom_ = std::clamp(z, kMinZoom, kMaxZoom);
}

void Workspace2D::adjust_zoom(float factor, float anchor_screen_x,
                              float anchor_screen_y, float viewport_w,
                              float viewport_h) {
  const float old_zoom = zoom_;
  const float new_zoom = std::clamp(old_zoom * factor, kMinZoom, kMaxZoom);
  if (std::abs(new_zoom - old_zoom) < 1e-6f) {
    return;
  }

  // Keep world point under the cursor stable across zoom.
  const float world_x =
      (anchor_screen_x - viewport_w * 0.5f) / old_zoom + pan_x_;
  const float world_y =
      (anchor_screen_y - viewport_h * 0.5f) / old_zoom + pan_y_;
  zoom_ = new_zoom;
  pan_x_ = world_x - (anchor_screen_x - viewport_w * 0.5f) / new_zoom;
  pan_y_ = world_y - (anchor_screen_y - viewport_h * 0.5f) / new_zoom;
}

std::vector<std::size_t> Workspace2D::sorted_draw_order() const {
  return draw_order(entities_);  // same order the runtime draws in
}

std::uint64_t Workspace2D::alloc_id() {
  return next_id_++;
}

std::string Workspace2D::strip_copy_suffix(const std::string& name) {
  // "Crate 3" -> "Crate" so duplicates count up instead of nesting suffixes.
  const std::size_t sp = name.find_last_of(' ');
  if (sp == std::string::npos || sp == 0 || sp + 1 >= name.size()) {
    return name;
  }
  for (std::size_t i = sp + 1; i < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') {
      return name;
    }
  }
  return name.substr(0, sp);
}

std::string Workspace2D::unique_name(const std::vector<Entity2D>& existing,
                                     std::string base) {
  bool clash = false;
  for (const auto& e : existing) {
    if (e.name == base) {
      clash = true;
      break;
    }
  }
  if (!clash) {
    return base;
  }
  for (int i = 2; i < 10000; ++i) {
    const std::string candidate = base + " " + std::to_string(i);
    bool found = false;
    for (const auto& e : existing) {
      if (e.name == candidate) {
        found = true;
        break;
      }
    }
    if (!found) {
      return candidate;
    }
  }
  return base + " x";
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
