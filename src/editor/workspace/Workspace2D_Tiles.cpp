#include "editor/workspace/Workspace2D.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

int Workspace2D::next_layer() const {
  return static_cast<int>(entities_.size());
}

std::uint64_t Workspace2D::create_tilemap(std::string name, int cols, int rows,
                                          int tile_size) {
  if (name.empty()) {
    name = "TileMap";
  }
  Entity2D e;
  e.id = alloc_id();
  e.name = unique_name(entities_, std::move(name));
  e.x = snap_always(32.0f + static_cast<float>(entities_.size() % 4) * 32.0f);
  e.y = snap_always(224.0f + static_cast<float>(entities_.size() % 3) * 32.0f);
  e.color[0] = 0.45f;
  e.color[1] = 0.45f;
  e.color[2] = 0.55f;
  e.color[3] = 1.0f;
  e.layer = 0;
  e.tilemap = TileMapData(cols, rows, tile_size);
  sync_tilemap_extent(e);
  entities_.push_back(std::move(e));
  const std::uint64_t id = entities_.back().id;
  select(id);
  return id;
}

bool Workspace2D::world_to_cell(std::uint64_t id, float wx, float wy, int* col,
                                int* row) const {
  const Entity2D* e = find(id);
  if (!e || !e->tilemap) {
    return false;
  }
  const float ts = static_cast<float>(e->tilemap->tile_size);
  const int c = static_cast<int>(std::floor((wx - e->x) / ts));
  const int r = static_cast<int>(std::floor((wy - e->y) / ts));
  if (col) *col = c;
  if (row) *row = r;
  return e->tilemap->in_bounds(c, r);
}

int Workspace2D::tile_at(std::uint64_t id, int col, int row) const {
  const Entity2D* e = find(id);
  if (!e || !e->tilemap || !e->tilemap->in_bounds(col, row)) {
    return -1;
  }
  return e->tilemap->at(col, row);
}

std::size_t Workspace2D::paint_tiles(std::uint64_t id, int col, int row,
                                     int tile_id, int brush) {
  Entity2D* e = find(id);
  if (!e || !e->tilemap) {
    return 0;
  }
  brush = std::clamp(brush, 1, 3);
  const int c0 = col - (brush - 1) / 2;
  const int r0 = row - (brush - 1) / 2;
  std::size_t changed = 0;
  for (int r = r0; r < r0 + brush; ++r) {
    for (int c = c0; c < c0 + brush; ++c) {
      if (e->tilemap->set(c, r, tile_id)) {
        ++changed;
      }
    }
  }
  return changed;
}

std::size_t Workspace2D::paint_line(std::uint64_t id, int c0, int r0, int c1,
                                    int r1, int tile_id, int brush) {
  // Bresenham so a fast drag never leaves holes between frames.
  std::size_t changed = 0;
  const int dx = std::abs(c1 - c0);
  const int dy = -std::abs(r1 - r0);
  const int sx = c0 < c1 ? 1 : -1;
  const int sy = r0 < r1 ? 1 : -1;
  int err = dx + dy;
  int c = c0;
  int r = r0;
  for (int guard = 0; guard < 8192; ++guard) {
    changed += paint_tiles(id, c, r, tile_id, brush);
    if (c == c1 && r == r1) {
      break;
    }
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      c += sx;
    }
    if (e2 <= dx) {
      err += dx;
      r += sy;
    }
  }
  return changed;
}

std::size_t Workspace2D::paint_autotile(std::uint64_t id, int col, int row, const AutoTileSet& set,
                                       int brush, bool erase) {
  Entity2D* e = find(id);
  if (!e || !e->tilemap) return 0;
  return ::ts::tombstone::paint_autotile(*e->tilemap, set, col, row, brush, erase);
}

std::size_t Workspace2D::paint_autotile_line(std::uint64_t id, int c0, int r0, int c1, int r1,
                                            const AutoTileSet& set, int brush, bool erase) {
  const int dx = std::abs(c1 - c0), dy = -std::abs(r1 - r0);
  const int sx = c0 < c1 ? 1 : -1, sy = r0 < r1 ? 1 : -1;
  int err = dx + dy, c = c0, r = r0;
  std::size_t changed = 0;
  while (true) {
    changed += paint_autotile(id, c, r, set, brush, erase);
    if (c == c1 && r == r1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      c += sx;
    }
    if (e2 <= dx) {
      err += dx;
      r += sy;
    }
  }
  return changed;
}

std::size_t Workspace2D::retile_tilemap(std::uint64_t id, const AutoTileSet& set) {
  Entity2D* e = find(id);
  if (!e || !e->tilemap) return 0;
  return retile_all(*e->tilemap, set);
}

std::size_t Workspace2D::flood_fill(std::uint64_t id, int col, int row,
                                    int tile_id) {
  Entity2D* e = find(id);
  if (!e || !e->tilemap || !e->tilemap->in_bounds(col, row)) {
    return 0;
  }
  TileMapData& tm = *e->tilemap;
  tile_id = std::clamp(tile_id, 0, TileMapData::kMaxTileId);
  const int target = tm.at(col, row);
  if (target == tile_id) {
    return 0;
  }
  std::size_t changed = 0;
  std::vector<std::pair<int, int>> stack;
  stack.emplace_back(col, row);
  while (!stack.empty()) {
    const auto [c, r] = stack.back();
    stack.pop_back();
    if (!tm.in_bounds(c, r) || tm.at(c, r) != target) {
      continue;
    }
    tm.set(c, r, tile_id);
    ++changed;
    stack.emplace_back(c + 1, r);
    stack.emplace_back(c - 1, r);
    stack.emplace_back(c, r + 1);
    stack.emplace_back(c, r - 1);
  }
  return changed;
}

std::size_t Workspace2D::fill_rect(std::uint64_t id, int c0, int r0, int c1,
                                   int r1, int tile_id) {
  Entity2D* e = find(id);
  if (!e || !e->tilemap) {
    return 0;
  }
  const int cmin = std::max(0, std::min(c0, c1));
  const int cmax = std::min(e->tilemap->cols - 1, std::max(c0, c1));
  const int rmin = std::max(0, std::min(r0, r1));
  const int rmax = std::min(e->tilemap->rows - 1, std::max(r0, r1));
  std::size_t changed = 0;
  for (int r = rmin; r <= rmax; ++r) {
    for (int c = cmin; c <= cmax; ++c) {
      if (e->tilemap->set(c, r, tile_id)) {
        ++changed;
      }
    }
  }
  return changed;
}

bool Workspace2D::resize_tilemap(std::uint64_t id, int cols, int rows) {
  Entity2D* e = find(id);
  if (!e || !e->tilemap) {
    return false;
  }
  cols = std::clamp(cols, 1, TileMapData::kMaxCells);
  rows = std::clamp(rows, 1, TileMapData::kMaxCells);
  if (cols == e->tilemap->cols && rows == e->tilemap->rows) {
    return false;
  }
  e->tilemap->resize(cols, rows);
  sync_tilemap_extent(*e);
  return true;
}

bool Workspace2D::set_tile_size(std::uint64_t id, int tile_size) {
  Entity2D* e = find(id);
  if (!e || !e->tilemap) {
    return false;
  }
  tile_size = std::clamp(tile_size, TileMapData::kMinTileSize,
                         TileMapData::kMaxTileSize);
  if (tile_size == e->tilemap->tile_size) {
    return false;
  }
  e->tilemap->tile_size = tile_size;
  sync_tilemap_extent(*e);
  return true;
}

bool Workspace2D::set_tileset(std::uint64_t id, std::string path) {
  Entity2D* e = find(id);
  if (!e || !e->tilemap || e->tilemap->tileset == path) {
    return false;
  }
  e->tilemap->tileset = std::move(path);
  return true;
}

std::uint64_t Workspace2D::first_tilemap() const {
  for (std::size_t idx : sorted_draw_order()) {
    if (entities_[idx].tilemap) {
      return entities_[idx].id;
    }
  }
  return 0;
}

std::optional<std::uint64_t> Workspace2D::pick_tilemap(float wx,
                                                       float wy) const {
  std::optional<std::uint64_t> hit;
  for (std::size_t idx : sorted_draw_order()) {
    const Entity2D& e = entities_[idx];
    if (e.tilemap && wx >= e.x && wy >= e.y && wx < e.x + e.w &&
        wy < e.y + e.h) {
      hit = e.id;
    }
  }
  return hit;
}

bool Workspace2D::set_sprite(std::uint64_t id,
                             std::optional<SpriteData> sprite) {
  Entity2D* e = find(id);
  if (!e || e->sprite == sprite) {
    return false;
  }
  e->sprite = std::move(sprite);
  return true;
}

std::uint64_t Workspace2D::create_sprite_entity(std::string name,
                                                std::string path, float x,
                                                float y, float w, float h) {
  if (name.empty()) {
    name = "Sprite";
  }
  Entity2D e;
  e.id = alloc_id();
  e.name = unique_name(entities_, std::move(name));
  e.x = snap_value(x);
  e.y = snap_value(y);
  e.w = std::clamp(w, 1.0f, 4096.0f);
  e.h = std::clamp(h, 1.0f, 4096.0f);
  for (float& c : e.color) {
    c = 1.0f;  // white tint = the image as drawn
  }
  e.layer = next_layer();
  SpriteData s;
  s.path = std::move(path);
  e.sprite = std::move(s);
  entities_.push_back(std::move(e));
  const std::uint64_t id = entities_.back().id;
  select(id);
  return id;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
