// Tile tools, sprites and assets for Editor2DScreen. Nothing in here needs an
// ImGui context, so --smoke drives these exact paths headless.
#include "editor/screens/Editor2DScreen.h"

#include "editor/assets/AssetLibrary.h"
#include "editor/tools/AutoTilePanel.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <utility>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

std::string tiles_text(std::size_t n) {
  return std::to_string(n) + (n == 1 ? " tile" : " tiles");
}

}  // namespace

const char* to_string(TileTool tool) {
  switch (tool) {
    case TileTool::Select:
      return "Select";
    case TileTool::Paint:
      return "Brush";
    case TileTool::Erase:
      return "Erase";
    case TileTool::Fill:
      return "Fill";
    case TileTool::Rect:
      return "Rect";
    case TileTool::Eyedropper:
      return "Eyedropper";
  }
  return "Select";
}

bool Editor2DScreen::commit_discrete(Workspace2D::Snapshot before,
                                     const std::string& label,
                                     const std::string& note_text) {
  if (!workspace_.commit_step(label, std::move(before))) {
    return false;
  }
  mark_dirty_and_autosave();
  if (!note_text.empty()) {
    note(note_text);
  }
  return true;
}

void Editor2DScreen::set_tool(TileTool tool) {
  if (stroke_active()) {
    end_paint_stroke();
  }
  tool_ = tool;
  if (tool == TileTool::Select) {
    return;
  }
  if (paint_target() == 0) {
    if (const std::uint64_t tm = workspace_.first_tilemap()) {
      workspace_.select(tm);
    }
  }
  if (const Entity2D* e = workspace_.find(paint_target())) {
    note(std::string(to_string(tool)) + " on " + e->name);
  } else {
    note(std::string(to_string(tool)) +
         " needs a TileMap. Edit > Create TileMap to stake one.");
  }
}

void Editor2DScreen::set_brush_tile(int tile_id) {
  brush_tile_ = std::clamp(tile_id, 1, TileMapData::kMaxTileId);
}

void Editor2DScreen::set_brush_size(int size) {
  brush_size_ = std::clamp(size, 1, 3);
}

std::uint64_t Editor2DScreen::paint_target() const {
  const Entity2D* e = workspace_.selected();
  return (e && e->tilemap) ? e->id : 0;
}

bool Editor2DScreen::begin_paint_stroke(std::uint64_t tilemap_id, bool erase) {
  const Entity2D* e = workspace_.find(tilemap_id);
  if (!e || !e->tilemap) {
    return false;
  }
  if (stroke_active()) {
    end_paint_stroke();
  }
  cancel_rename();
  flush_pending_edit();
  workspace_.begin_edit(erase ? "Erase" : "Paint");
  edit_source_ = EditSource::Paint;
  stroke_id_ = tilemap_id;
  stroke_erase_ = erase;
  stroke_has_last_ = false;
  // A terrain brush on a fresh TileMap adopts the terrain's tileset.
  if (const AutoTileSet* t = AutoTilePanel::get().brush(project_.path)) {
    Entity2D* m = workspace_.find(tilemap_id);
    if (m && m->tilemap && m->tilemap->count_nonempty() == 0 && m->tilemap->tileset != t->tileset) {
      m->tilemap->tileset = t->tileset;
      m->tilemap->tile_size = t->tile_size;
    }
  }
  stroke_cells_ = 0;
  return true;
}

std::size_t Editor2DScreen::stroke_to_cell(int col, int row) {
  if (!stroke_active()) {
    return 0;
  }
  if (stroke_has_last_ && col == stroke_last_col_ && row == stroke_last_row_) {
    return 0;
  }
  const int tile = stroke_erase_ ? 0 : brush_tile_;
  const AutoTileSet* terrain = AutoTilePanel::get().brush(project_.path);
  const std::size_t n =
      terrain ? (stroke_has_last_
                     ? workspace_.paint_autotile_line(stroke_id_, stroke_last_col_, stroke_last_row_, col, row,
                                                      *terrain, brush_size_, stroke_erase_)
                     : workspace_.paint_autotile(stroke_id_, col, row, *terrain, brush_size_, stroke_erase_))
      : stroke_has_last_
          ? workspace_.paint_line(stroke_id_, stroke_last_col_,
                                  stroke_last_row_, col, row, tile,
                                  brush_size_)
          : workspace_.paint_tiles(stroke_id_, col, row, tile, brush_size_);
  stroke_has_last_ = true;
  stroke_last_col_ = col;
  stroke_last_row_ = row;
  stroke_cells_ += n;
  if (n > 0) {
    mark_dirty();
  }
  return n;
}

bool Editor2DScreen::end_paint_stroke() {
  if (!stroke_active()) {
    return false;
  }
  const std::size_t n = stroke_cells_;
  const bool erase = stroke_erase_;
  stroke_id_ = 0;
  stroke_has_last_ = false;
  stroke_cells_ = 0;
  bool pushed = false;
  if (edit_source_ == EditSource::Paint) {
    workspace_.set_edit_label((erase ? "Erase " : "Paint ") + tiles_text(n));
    pushed = flush_pending_edit();
  }
  if (pushed) {
    mark_dirty_and_autosave();
    note((erase ? "Cleared " : "Painted ") + tiles_text(n));
  }
  return pushed;
}

void Editor2DScreen::cancel_paint_stroke() {
  if (!stroke_active()) {
    return;
  }
  stroke_id_ = 0;
  stroke_has_last_ = false;
  stroke_cells_ = 0;
  if (edit_source_ == EditSource::Paint) {
    workspace_.revert_edit();
    edit_source_ = EditSource::None;
  }
  note("Stroke called off");
}

std::size_t Editor2DScreen::fill_at(std::uint64_t tilemap_id, int col,
                                    int row) {
  Workspace2D::Snapshot before = prepare_edit();
  const std::size_t n =
      workspace_.flood_fill(tilemap_id, col, row, brush_tile_);
  if (n == 0) {
    return 0;
  }
  commit_discrete(std::move(before), "Fill " + tiles_text(n),
                  "Filled " + tiles_text(n));
  return n;
}

std::size_t Editor2DScreen::fill_rect_cells(std::uint64_t tilemap_id, int c0,
                                            int r0, int c1, int r1,
                                            bool erase) {
  Workspace2D::Snapshot before = prepare_edit();
  const std::size_t n = workspace_.fill_rect(tilemap_id, c0, r0, c1, r1,
                                             erase ? 0 : brush_tile_);
  if (n == 0) {
    return 0;
  }
  commit_discrete(std::move(before),
                  (erase ? "Erase Rect " : "Rect ") + tiles_text(n),
                  (erase ? "Cleared " : "Laid ") + tiles_text(n));
  return n;
}

bool Editor2DScreen::eyedrop(std::uint64_t tilemap_id, int col, int row) {
  const int t = workspace_.tile_at(tilemap_id, col, row);
  if (t <= 0) {
    note("Nothing to pick there. Empty ground.");
    return false;
  }
  set_brush_tile(t);
  note("Picked tile " + std::to_string(t));
  return true;
}

bool Editor2DScreen::resize_tilemap(std::uint64_t tilemap_id, int cols,
                                    int rows) {
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.resize_tilemap(tilemap_id, cols, rows)) {
    return false;
  }
  const Entity2D* e = workspace_.find(tilemap_id);
  return commit_discrete(
      std::move(before), "Resize TileMap",
      "TileMap now " + std::to_string(e->tilemap->cols) + " x " +
          std::to_string(e->tilemap->rows) + " cells");
}

bool Editor2DScreen::set_tile_size(std::uint64_t tilemap_id, int tile_size) {
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.set_tile_size(tilemap_id, tile_size)) {
    return false;
  }
  const Entity2D* e = workspace_.find(tilemap_id);
  return commit_discrete(
      std::move(before), "Tile Size",
      "Tile size " + std::to_string(e->tilemap->tile_size) + " px");
}

bool Editor2DScreen::set_tileset(std::uint64_t tilemap_id,
                                 const std::string& rel_path) {
  Workspace2D::Snapshot before = prepare_edit();
  if (!workspace_.set_tileset(tilemap_id, rel_path)) {
    return false;
  }
  return commit_discrete(std::move(before), "Set Tileset",
                         rel_path.empty()
                             ? std::string("Back to the built-in palette")
                             : "Tileset " + assets::file_name(rel_path));
}

std::uint64_t Editor2DScreen::create_tilemap() {
  cancel_rename();
  Workspace2D::Snapshot before = prepare_edit();
  const std::uint64_t id = workspace_.create_tilemap("TileMap", 16, 8, 32);
  if (id == 0) {
    return 0;
  }
  const Entity2D* e = workspace_.find(id);
  commit_discrete(std::move(before), "Create TileMap",
                  "Staked " + (e ? e->name : std::string("TileMap")));
  return id;
}

bool Editor2DScreen::tileset_ready(const Entity2D& e) {
  if (!e.tilemap || e.tilemap->tileset.empty()) {
    return false;
  }
  const TextureInfo& t = texture(e.tilemap->tileset);
  return t.ok && t.width >= e.tilemap->tile_size &&
         t.height >= e.tilemap->tile_size;
}

int Editor2DScreen::palette_count(const Entity2D& e) {
  if (!e.tilemap) {
    return 0;
  }
  if (tileset_ready(e)) {
    const TextureInfo& t = texture(e.tilemap->tileset);
    const int per_row = t.width / e.tilemap->tile_size;
    const int per_col = t.height / e.tilemap->tile_size;
    return std::min(per_row * per_col, TileMapData::kMaxTileId);
  }
  return kBuiltinTileCount;
}

// --- Sprites / assets ---------------------------------------------------------

std::string Editor2DScreen::asset_path(const std::string& rel) const {
  return assets::resolve(project_.path, rel);
}

const TextureInfo& Editor2DScreen::texture(const std::string& rel) {
  return textures_.get(asset_path(rel));
}

SpriteState Editor2DScreen::sprite_state(const Entity2D& e) {
  if (!e.sprite) {
    return SpriteState::None;
  }
  if (e.sprite->path.empty()) {
    return SpriteState::Missing;
  }
  return texture(e.sprite->path).ok ? SpriteState::Ready
                                    : SpriteState::Missing;
}

bool Editor2DScreen::assign_sprite(std::uint64_t id,
                                   const std::string& rel_path) {
  const Entity2D* e = workspace_.find(id);
  if (!e) {
    return false;
  }
  Workspace2D::Snapshot before = prepare_edit();
  std::optional<SpriteData> next;
  if (!rel_path.empty()) {
    SpriteData s = e->sprite.value_or(SpriteData{});
    s.path = rel_path;
    next = std::move(s);
  }
  if (!workspace_.set_sprite(id, std::move(next))) {
    return false;
  }
  return commit_discrete(
      std::move(before), rel_path.empty() ? "Remove Sprite" : "Assign Sprite",
      rel_path.empty() ? "Sprite pulled from " + e->name
                       : "Saddled " + workspace_.find(id)->name + " with " +
                             assets::file_name(rel_path));
}

std::uint64_t Editor2DScreen::create_sprite_at(const std::string& rel_path,
                                               float wx, float wy) {
  if (rel_path.empty()) {
    return 0;
  }
  cancel_rename();
  const TextureInfo& tex = texture(rel_path);
  float w = tex.ok ? static_cast<float>(tex.width) : 64.0f;
  float h = tex.ok ? static_cast<float>(tex.height) : 64.0f;
  // A sheet with a .anim.json beside it rides in animated, one frame big.
  const std::string set_rel = anim_set_for_image(rel_path);
  const AnimLibrary::Entry* anim = set_rel.empty() ? nullptr : anim_entry(set_rel);
  const AnimClip* first =
      anim && anim->ok ? anim->set.find(anim->set.start_clip()) : nullptr;
  AnimRect frame;
  if (first) {
    frame = anim->set.frame_rect(*first, 0);
    w = static_cast<float>(frame.w);
    h = static_cast<float>(frame.h);
  }
  Workspace2D::Snapshot before = prepare_edit();
  std::string name = assets::file_stem(rel_path);
  const std::uint64_t id = workspace_.create_sprite_entity(
      name.empty() ? "Sprite" : name, rel_path, wx - w * 0.5f, wy - h * 0.5f,
      w, h);
  if (id == 0) {
    return 0;
  }
  if (first) {
    Entity2D* e = workspace_.find(id);
    e->sprite->use_src_rect = true;  // first frame when the set goes missing
    e->sprite->src_x = frame.x;
    e->sprite->src_y = frame.y;
    e->sprite->src_w = frame.w;
    e->sprite->src_h = frame.h;
    e->animator = AnimatorData{};
    e->animator->set = set_rel;
  }
  commit_discrete(std::move(before),
                  first ? "Create Animated Sprite" : "Create Sprite",
                  "Staked " + workspace_.find(id)->name +
                      (first ? " (animated, " +
                                   std::to_string(anim->set.clips.size()) +
                                   " clips)"
                       : tex.ok ? std::string()
                                : std::string(" (image missing)")));
  return id;
}

void Editor2DScreen::refresh_assets() {
  assets_ = assets::list_images(project_.path);
  sounds_ = assets::list_sounds(project_.path);
  assets_refresh_time_ = now_seconds();
}

bool Editor2DScreen::import_asset(const std::string& source_path,
                                  std::string* rel_out) {
  std::string rel;
  std::string err;
  if (!assets::import_image(project_.path, source_path, &rel, &err)) {
    note("Import failed: " + err);
    return false;
  }
  textures_.invalidate(asset_path(rel));
  refresh_assets();
  note("Loaded " + assets::file_name(rel) + " into the Supply Wagon");
  if (rel_out) {
    *rel_out = rel;
  }
  return true;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
