#pragma once

#include "editor/workspace/TileMap.h"
#include "scene/SceneData.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

// Entity2D and its components live in scene/SceneData.h (shared with the
// runtime); the editor works on the same structs.
using ::ts::tombstone::Camera2DData;
using ::ts::tombstone::ColliderData;
using ::ts::tombstone::Entity2D;
using ::ts::tombstone::PlayerControllerData;
using ::ts::tombstone::SpawnPointData;
using ::ts::tombstone::TileSolidity;

// Flat hierarchy under a conceptual root "Scene" node.
// Persisted per project as scene.json via scene_io (load/save).
class Workspace2D {
 public:
  Workspace2D();

  void reset_defaults();  // seed Camera2D / Player / TileMap placeholders

  // Replace full scene state (used by scene.json load).
  void replace_scene(std::vector<Entity2D> entities,
                     std::optional<std::uint64_t> selected_id, float pan_x,
                     float pan_y, float zoom, bool show_grid);

  const std::vector<Entity2D>& entities() const { return entities_; }
  std::vector<Entity2D>& entities() { return entities_; }

  // --- Selection -----------------------------------------------------------
  // selected_id() is the *primary* selection (Inspector target). selection()
  // holds every selected id (primary included), in pick order.
  std::optional<std::uint64_t> selected_id() const { return selected_id_; }
  const std::vector<std::uint64_t>& selection() const { return selection_; }
  std::size_t selection_count() const { return selection_.size(); }
  bool is_selected(std::uint64_t id) const;
  void select(std::optional<std::uint64_t> id);  // replace with single / none
  void add_to_selection(std::uint64_t id);       // also becomes primary
  void toggle_selection(std::uint64_t id);       // Ctrl+click
  void set_selection(const std::vector<std::uint64_t>& ids,
                     std::optional<std::uint64_t> primary = std::nullopt);
  void select_all();
  void clear_selection();
  // Select entities whose rect intersects the world-space box. additive keeps
  // the current selection. Returns number of entities hit by the box.
  std::size_t select_in_rect(float x0, float y0, float x1, float y1,
                             bool additive);
  // Topmost entity (highest layer, then latest id) under a world point.
  std::optional<std::uint64_t> pick(float wx, float wy) const;

  Entity2D* find(std::uint64_t id);
  const Entity2D* find(std::uint64_t id) const;
  Entity2D* selected();
  const Entity2D* selected() const;

  // Create under the root Scene. Returns new id, or 0 on failure.
  std::uint64_t create_entity(std::string name = "Entity");
  bool rename_entity(std::uint64_t id, std::string name);
  bool delete_entity(std::uint64_t id);

  // --- Editing tools (operate on the whole selection) ----------------------
  // Copies every selected entity (offset by one grid cell when snapping, else
  // 16 px), selects the copies and returns their ids in order.
  // offset = false leaves the copies on top of the originals (Alt+drag).
  std::vector<std::uint64_t> duplicate_selection(bool offset = true);
  // Removes every selected entity. Returns how many were removed.
  std::size_t delete_selection();
  // Arrow-key nudge. dir_x/dir_y in {-1,0,1}. Step = grid (x4 when large) with
  // snap on, else 1 px (10 px when large). With snap on, positions land on
  // the grid. Returns true if anything moved.
  bool nudge_selection(int dir_x, int dir_y, bool large);
  // Snap the selection's positions to the grid. Returns true if any changed.
  bool snap_selection_to_grid();

  // Drag-move session: begin captures start positions, update applies a
  // world-space offset from those starts (snapping the primary to the grid
  // and moving the rest by the same delta), end reports whether anything
  // actually moved.
  void begin_move();
  void update_move(float dx, float dy);
  bool end_move();
  void cancel_move();  // restore start positions (Esc during a drag)
  bool move_active() const { return move_active_; }

  // --- Editor tools (Workspace2D_Tools.cpp) ---------------------------------
  // Hierarchy: Entity2D::parent links (world-space positions; children
  // follow their parent's moves, resizes and turns). The Hierarchy lists
  // roots in entities() order, each child under its parent in the same
  // order. Moving / deleting / duplicating a selected entity takes its
  // whole subtree along.
  std::vector<std::uint64_t> children_of(std::uint64_t id) const;
  std::vector<std::uint64_t> descendants_of(std::uint64_t id) const;  // depth-first
  bool is_ancestor(std::uint64_t ancestor, std::uint64_t id) const;
  int depth_of(std::uint64_t id) const;
  // Selected entities whose ancestors are not selected (what moves).
  std::vector<std::uint64_t> selection_roots() const;
  // Hidden / locked here or on any ancestor.
  bool effectively_hidden(const Entity2D& e) const;
  bool effectively_locked(const Entity2D& e) const;
  // Reparent (0 = root). Refuses loops and unknown ids. True if changed.
  bool set_parent(std::uint64_t id, std::uint64_t parent);
  // Hierarchy drag: put `id` (with its subtree) before / after `target`
  // (same parent as target) or inside it (last child). True if changed.
  enum class HierDrop { Before, After, Inside };
  bool hierarchy_drop(std::uint64_t id, std::uint64_t target, HierDrop where);
  // New "Group" around the selection roots (bounding box, transparent,
  // under their common parent) holding them; selected. Returns its id.
  std::uint64_t group_selection(const std::string& name = "Group");
  // Children of each selected entity move up one level and the (now
  // empty) parents are removed. Returns entities freed.
  std::size_t ungroup_selection();
  // Lock / hide every selected entity. True if anything changed.
  bool set_selection_locked(bool locked);
  bool set_selection_hidden(bool hidden);
  bool set_locked(std::uint64_t id, bool locked);
  bool set_hidden(std::uint64_t id, bool hidden);

  // Snap to other entities: edges / centres of everything visible that is
  // not moving, within `threshold` world px. Grid snapping (when on) runs
  // first; an object guide wins when closer. Lines for the viewport.
  struct SnapGuide {
    bool vertical = true;  // x = pos (else y = pos)
    float pos = 0.0f;
    float from = 0.0f, to = 0.0f;  // extent along the other axis
  };
  void set_object_snap(bool on) { object_snap_ = on; }
  bool object_snap() const { return object_snap_; }
  void set_snap_threshold(float world_px) { snap_threshold_ = world_px; }
  const std::vector<SnapGuide>& guides() const { return guides_; }
  void clear_guides() { guides_.clear(); }
  // Offset that snaps a box (minus `moving` ids) to the nearest guides.
  void snap_box_to_objects(float x0, float y0, float x1, float y1,
                           const std::vector<std::uint64_t>& moving, float* dx,
                           float* dy, std::vector<SnapGuide>* guides) const;

  // Resize / rotate the primary selection with the viewport handles.
  // Handle (hx, hy) in {-1, 0, 1}: which side moves (corner = both).
  // keep_aspect (Shift) keeps the start ratio. Children scale along.
  // begin -> update (any number, absolute mouse) -> end; Esc cancels.
  bool begin_transform();
  void resize_to(int hx, int hy, float wx, float wy, bool keep_aspect);
  // Turn to start + delta degrees (step > 0 rounds to it). Children orbit.
  void rotate_to(float delta_deg, float step);
  bool end_transform();     // true if anything changed
  void cancel_transform();  // restore the start
  bool transform_active() const { return xform_active_; }

  // Align (multi-select; to the selection bounds) and distribute (3+, equal
  // gaps between the edges). Subtrees move with their roots.
  enum class Align { Left, CenterX, Right, Top, CenterY, Bottom };
  bool align_selection(Align how);
  bool distribute_selection(bool horizontal);

  // Clipboard: the selection roots and their subtrees as scene.json text
  // (entities only). paste_json adds copies with fresh ids (parents inside
  // the clip kept, others under the root), offset when `offset`, selects
  // them and returns the new root ids. Bad text pastes nothing.
  std::string copy_selection_json() const;
  std::vector<std::uint64_t> paste_json(const std::string& text, bool offset);

  // World-space bounds of the selection (or every visible entity when
  // nothing is selected); false when empty.
  bool selection_bounds(float* x0, float* y0, float* x1, float* y1) const;
  // Centre the view on the selection and zoom to fit (F).
  bool frame_selection(float viewport_w, float viewport_h);

  // Viewport camera (world space).
  float pan_x() const { return pan_x_; }
  float pan_y() const { return pan_y_; }
  float zoom() const { return zoom_; }
  void set_pan(float x, float y);
  void add_pan(float dx, float dy);
  void set_zoom(float z);
  void adjust_zoom(float factor, float anchor_screen_x, float anchor_screen_y,
                   float viewport_w, float viewport_h);

  bool show_grid() const { return show_grid_; }
  void set_show_grid(bool v) { show_grid_ = v; }
  float grid_size() const { return grid_size_; }
  void set_grid_size(float g);  // clamped to [kMinGrid, kMaxGrid]
  bool snap_enabled() const { return snap_enabled_; }
  void set_snap_enabled(bool v) { snap_enabled_ = v; }
  // Round to the nearest grid line (identity when snap is off).
  float snap_value(float v) const;
  // Round to the nearest grid line regardless of the snap toggle.
  float snap_always(float v) const;
  // Snap a size to whole grid cells (never below one cell) when snap is on.
  float snap_extent(float v) const;

  static constexpr float kMinGrid = 2.0f;
  static constexpr float kMaxGrid = 512.0f;

  // --- TileMaps (Workspace2D_Tiles.cpp) -------------------------------------
  // New TileMap entity (selected). Returns id.
  std::uint64_t create_tilemap(std::string name = "TileMap", int cols = 16,
                               int rows = 8, int tile_size = 32);
  // Cell under a world point (floor). Returns true when inside the grid.
  bool world_to_cell(std::uint64_t id, float wx, float wy, int* col,
                     int* row) const;
  // Tile id at a cell; -1 when id is not a TileMap or the cell is outside.
  int tile_at(std::uint64_t id, int col, int row) const;
  // Square brush (size 1..3, centred on the cell). Returns cells changed.
  std::size_t paint_tiles(std::uint64_t id, int col, int row, int tile_id,
                          int brush = 1);
  // Brush along a line of cells (no gaps on fast drags).
  std::size_t paint_line(std::uint64_t id, int c0, int r0, int c1, int r1,
                         int tile_id, int brush = 1);
  // 4-connected bucket fill of the region matching the clicked tile.
  std::size_t flood_fill(std::uint64_t id, int col, int row, int tile_id);
  std::size_t fill_rect(std::uint64_t id, int c0, int r0, int c1, int r1,
                        int tile_id);
  bool resize_tilemap(std::uint64_t id, int cols, int rows);
  bool set_tile_size(std::uint64_t id, int tile_size);
  bool set_tileset(std::uint64_t id, std::string path);
  // Keep w/h equal to the grid extent.
  static void sync_tilemap_extent(Entity2D& e) {
    ::ts::tombstone::sync_tilemap_extent(e);
  }
  // First TileMap in draw order (0 when none).
  std::uint64_t first_tilemap() const;
  // Topmost TileMap under a world point.
  std::optional<std::uint64_t> pick_tilemap(float wx, float wy) const;

  // Which tile ids are solid, per tileset ("" = built-in palette). Part of
  // the undo history and of scene.json (v4 "tile_solidity").
  const TileSolidity& tile_solidity() const { return tile_solidity_; }
  void set_tile_solidity(TileSolidity solidity);
  // Scene lighting (scene.json v9 "lighting"): ambient, day-night, lightmap
  // resolution. Saved with the scene; not part of undo.
  const fx::Lighting2D& lighting() const { return lighting_; }
  void set_lighting(const fx::Lighting2D& lighting);
  // True when the flag changed.
  bool set_tile_solid(const std::string& tileset, int tile_id, bool solid);

  // --- Collision -------------------------------------------------------------
  // nullopt removes the collider. Returns true if anything changed.
  bool set_collider(std::uint64_t id, std::optional<ColliderData> collider);

  // --- Sprites ---------------------------------------------------------------
  // nullopt removes the sprite. Returns true if anything changed.
  bool set_sprite(std::uint64_t id, std::optional<SpriteData> sprite);
  // New sprite entity (white tint, selected). Returns id.
  std::uint64_t create_sprite_entity(std::string name, std::string path,
                                     float x, float y, float w, float h);

  // Sorted copy of entity indices by layer ascending (stable by id).
  std::vector<std::size_t> sorted_draw_order() const;

  // --- Undo / redo (in-memory, snapshot based) -----------------------------
  // A snapshot is the entity list, tile solidity and selection. Camera, grid
  // and snap are view settings and are not part of history.
  struct Snapshot {
    std::vector<Entity2D> entities;
    TileSolidity tile_solidity;
    std::optional<std::uint64_t> selected_id;
    std::vector<std::uint64_t> selection;
  };
  static constexpr std::size_t kMaxHistory = 200;

  Snapshot snapshot() const;
  // Restore entities + selection. Ids are never reused (next id only grows).
  void restore(const Snapshot& s);
  // True when the entity lists and tile solidity match field for field
  // (selection ignored).
  bool same_entities(const Snapshot& s) const;

  // Push one committed step whose pre-edit state is `before`. Skipped when
  // the entities did not change (selection-only changes are not steps).
  // A pushed step clears the redo stack. Returns true if a step was pushed.
  bool commit_step(std::string label, Snapshot before);

  // Coalesced edit (drag, Inspector field, held-arrow nudge): begin captures
  // the before-state once, later begins are ignored while open, commit pushes
  // a single step for everything that changed in between.
  void begin_edit(std::string label);
  void begin_edit(std::string label, Snapshot before);
  void set_edit_label(std::string label);
  bool commit_edit();
  void cancel_edit();  // drop the pending before-state (keeps current state)
  // Drop the pending edit AND restore its before-state (Esc mid-stroke).
  bool revert_edit();
  bool edit_open() const { return pending_.has_value(); }
  // Open and has actually changed entities (i.e. commit would push a step).
  bool edit_changed() const { return pending_ && !same_entities(*pending_); }
  const std::string& edit_label() const { return pending_label_; }

  bool can_undo() const { return !undo_.empty(); }
  bool can_redo() const { return !redo_.empty(); }
  std::size_t undo_count() const { return undo_.size(); }
  std::size_t redo_count() const { return redo_.size(); }
  // Label of the step Undo / Redo would apply ("" when empty).
  const std::string& undo_label() const;
  const std::string& redo_label() const;
  // Commits any open coalesced edit first. label_out receives the step name.
  bool undo(std::string* label_out = nullptr);
  bool redo(std::string* label_out = nullptr);
  void clear_history();

 private:
  std::uint64_t alloc_id();
  int next_layer() const;
  void sync_next_id_from_entities();
  static std::string strip_copy_suffix(const std::string& name);
  static std::string unique_name(const std::vector<Entity2D>& existing,
                                 std::string base);

  void prune_selection();
  float snap_step(float v, int dir, float step) const;

  struct MoveStart {
    std::uint64_t id;
    float x;
    float y;
  };
  // Selection roots plus every descendant (each once), skipping locked.
  std::vector<std::uint64_t> moving_set() const;
  void move_subtree(std::uint64_t id, float dx, float dy);

  struct HistoryStep {
    std::string label;
    Snapshot state;  // state to return to when this step is applied
  };

  std::deque<HistoryStep> undo_;
  std::deque<HistoryStep> redo_;
  std::optional<Snapshot> pending_;
  std::string pending_label_;

  std::vector<Entity2D> entities_;
  TileSolidity tile_solidity_;
  fx::Lighting2D lighting_;
  std::optional<std::uint64_t> selected_id_;
  std::vector<std::uint64_t> selection_;
  std::vector<MoveStart> move_starts_;
  bool move_active_ = false;
  bool move_changed_ = false;
  bool snap_enabled_ = false;
  std::uint64_t next_id_ = 1;
  float pan_x_ = 0.0f;
  float pan_y_ = 0.0f;
  float zoom_ = 1.0f;
  bool show_grid_ = true;
  float grid_size_ = 32.0f;
  // Editor tools.
  std::vector<std::uint64_t> move_roots_;  // primary-first roots of a drag
  bool object_snap_ = true;
  float snap_threshold_ = 6.0f;
  std::vector<SnapGuide> guides_;
  bool xform_active_ = false;
  std::vector<Entity2D> xform_start_;  // primary first, then its subtree
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
