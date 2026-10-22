#pragma once

#include "audio/SoundStage.h"
#include "debugtools/DebugTools.h"
#include "editor/plugins/ProfilerPanels.h"
#include "editor/ProjectInfo.h"
#include "editor/assets/AssetBrowser.h"
#include "editor/assets/PrefabLibrary.h"
#include "editor/assets/TextureCache.h"
#include "editor/vault/VaultPanel.h"
#include "editor/console/TelegraphLog.h"
#include "editor/plugins/EditorPlugins.h"
#include "editor/plugins/PluginsPanel.h"
#include "editor/scripting/ScriptComponents.h"
#include "editor/transform/Gizmo2D.h"
#include "editor/transform/ModalTransform.h"
#include "editor/screens/IScreen.h"
#include "editor/ui/FolderBrowser.h"
#include "editor/ui/PlayUi.h"
#include "editor/ux/EditorUx.h"
#include "editor/workspace/Workspace2D.h"
#include "mp/LocalPosse.h"
#include "net/SimTransport.h"
#include "runtime/PlaySession.h"
#include "runtime/Script.h"
#include "scene/Animation.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace ts {
namespace tombstone {
namespace editor {

// Viewport tools. Select keeps the original pick / drag / marquee behaviour;
// the rest paint the selected TileMap.
enum class TileTool { Select, Paint, Erase, Fill, Rect, Eyedropper };
const char* to_string(TileTool tool);

// Draw one tile: a slice of `tileset` (when non-null and id <= count) or the
// built-in palette colour. alpha scales the result (previews).
void draw_tile_quad(ImDrawList* draw, const TextureInfo* tileset,
                    int tile_size, int palette_count, int tile_id,
                    const ImVec2& p0, const ImVec2& p1, float alpha = 1.0f);
// Red corner wedge on a palette swatch whose tile blocks riders.
void draw_solid_marker(ImDrawList* draw, const ImVec2& p0, const ImVec2& p1);

// How a sprite entity will draw this frame.
enum class SpriteState { None, Ready, Missing };

// What an entity shows in the edit viewport: a texture and its UVs (the
// sprite's source rect, or the animator's current frame). flip already
// applied (u0 > u1 when mirrored).
struct EntityImage {
  const TextureInfo* texture = nullptr;
  float u0 = 0.0f;
  float v0 = 0.0f;
  float u1 = 1.0f;
  float v1 = 1.0f;
  bool animated = false;  // the frame comes from an animation set
};


// 2D editor workspace: Hierarchy list, Viewport2D canvas, Inspector
// properties, Tile Palette, Supply Wagon (assets), Scripts and Telegraph
// (console) panels.
// Workspace is loaded from / saved to <project>/scene.json.
class Editor2DScreen final : public IScreen {
 public:
  explicit Editor2DScreen(ProjectInfo project);

  AppState state() const override { return AppState::Editor2D; }

  void on_enter() override;
  void on_exit() override;
  AppState on_update(float delta_seconds) override;

  void request_quit();
  void request_back_to_projects();
  // Quit / Back to Projects from the UI: asks first when the latest changes
  // didn't reach disk (autosave failed or the scene is read-only).
  void leave_or_ask(bool quit);
  // Plain dirty edits are written on leave anyway (on_exit saves), so only a
  // failed save counts.
  bool has_unsaved() const { return dirty_ && (!last_save_error_.empty() || scene_locked_); }
  // Field Guide: action registry + command palette (Ctrl+Shift+P), panel /
  // Settings / recent entries; --smoke-ux drives it.
  ux::EditorUx& field_guide() { return ux_; }
  bool hierarchy_shown() const { return show_hierarchy_; }

  const ProjectInfo& project() const { return project_; }
  Workspace2D& workspace() { return workspace_; }
  const Workspace2D& workspace() const { return workspace_; }

  // Persist current workspace to scene.json. Clears dirty on success.
  bool save_scene(std::string* error_out = nullptr);
  bool is_dirty() const { return dirty_; }
  const std::string& scene_path() const { return scene_path_; }

  // Editing tools shared by hotkeys, menus, toolbar and hierarchy buttons.
  // Each autosaves scene.json like the other workspace edits.
  std::size_t duplicate_selected();  // Ctrl+D
  std::size_t delete_selected();     // Delete
  bool nudge_selected(int dir_x, int dir_y, bool large);  // arrows (+Shift)
  std::uint64_t create_entity(std::string name = "Entity");
  bool snap_selected_to_grid();
  void reset_scene_placeholders();

  // --- Prefabs (Editor2DScreen_Prefabs.cpp) ---------------------------------
  // Same model as the 3D editor (docs/prefabs.md). Each is one undo step +
  // autosave; asset files written by Make / Apply / Edit follow undo.
  std::string save_selection_as_prefab(const std::string& name, std::string* error = nullptr);
  std::uint64_t place_prefab(const std::string& rel, float x, float y);
  bool prefab_apply(std::uint64_t id, const std::string& key = {}, bool whole_instance = false);
  bool prefab_revert(std::uint64_t id, const std::string& key = {}, bool whole_instance = false);
  bool prefab_unpack(std::uint64_t id);
  bool prefab_edit_asset(const std::string& rel, const prefab::Asset& asset);
  std::size_t prefab_poll();
  std::vector<std::uint64_t> prefab_instance_members(std::uint64_t id) const;
  PrefabLibrary& prefabs() { return prefabs_; }
  // Asset Browser (shared with the 3D editor; docs/assets.md).
  AssetBrowser& asset_browser() { return asset_browser_; }
  VaultPanel& vault_panel() { return vault_panel_; }
  // Reload scene.json from disk (after the browser rewrote references).
  bool reload_scene_from_disk();

  // Undo / redo (Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z, Edit menu, toolbar). Each
  // restores entities + selection, autosaves scene.json and notes the step.
  bool undo();
  bool redo();

  // Coalesced edits. These are what the viewport drag and Inspector widgets
  // call; public so --smoke can drive the same paths without a window.
  // Drag-move: one undo step per whole drag.
  void begin_drag_move();
  bool end_drag_move();     // commits "Move N" + autosave if anything moved
  void cancel_drag_move();  // Esc: restore start positions, no step
  // Inspector field: before-state captured on activation, one step on
  // deactivation-after-edit.
  void begin_inspector_edit(const char* label);
  bool end_inspector_edit();
  // Commit whatever coalesced edit is open (held-arrow nudge, drag,
  // Inspector). Returns true if an undo step was pushed.
  bool flush_pending_edit();

  // --- Editor tools (Editor2DScreen_Tools.cpp) --------------------------------
  // Each is one undo step and autosaves, like the rest. Locked while playing.
  // Clipboard: scene.json text of the selection (and subtrees), shared
  // across scenes / projects through the system clipboard.
  bool copy_selected();
  std::size_t cut_selected();
  std::size_t paste();  // offset copies, selected
  static const std::string& clipboard_text();
  std::uint64_t group_selected();     // Ctrl+G
  std::size_t ungroup_selected();     // Ctrl+Shift+G
  bool set_entity_locked(std::uint64_t id, bool locked);
  bool set_entity_hidden(std::uint64_t id, bool hidden);
  bool lock_selected(bool locked);    // Ctrl+L / Ctrl+Shift+L
  bool hide_selected(bool hidden);    // H / Shift+H
  bool reparent(std::uint64_t id, std::uint64_t parent);
  bool hierarchy_drop(std::uint64_t id, std::uint64_t target,
                      Workspace2D::HierDrop where);
  bool align_selected(Workspace2D::Align how);
  bool distribute_selected(bool horizontal);
  // F: centre + zoom the viewport on the selection (everything when none).
  bool frame_selected();
  // "Collider" / "Player" / "Camera" / "Spawn". False if present / unknown.
  bool add_component(std::uint64_t id, const std::string& kind);
  // Viewport handles on the primary selection. hx / hy in {-1, 0, 1};
  // rotate = the turn knob. update takes the mouse in world space; shift
  // keeps the aspect (resize) or steps 15 degrees (turn). One undo step.
  bool begin_handle_drag(int hx, int hy, bool rotate, float wx, float wy);
  void update_handle_drag(float wx, float wy, bool shift);
  bool end_handle_drag();
  void cancel_handle_drag();
  // Alt+drag: copies of the selection on the spot, then a normal drag
  // moves the copies. One "Duplicate N" step; Esc takes the copies back.
  bool begin_duplicate_drag();
  // --- Cameras + attach tools (Editor2DScreen_Attach.cpp) -------------------
  // docs/camera.md, docs/transforms.md. Each edit is one undo step on the
  // selection roots (minus the target) and refuses while playing.
  std::uint64_t create_camera_2d();  // at the view centre, follows the rider
  bool make_active_camera_2d(std::uint64_t id);
  std::uint64_t play_camera_2d() const;  // highest-priority active (0 = none)
  bool attach_selected_2d(std::uint64_t target, bool snap);
  bool detach_selected_2d();
  bool follow_selected_2d(std::uint64_t target, bool position, bool rotation);
  bool remove_follow_selected_2d();
  bool snap_selected_to_2d(std::uint64_t target);
  bool copy_transform_to_selected_2d(std::uint64_t from, bool position, bool rotation, bool scale);
  bool align_selected_to_surface_2d();
  // Eyedropper (same actions as 3D: attach_keep, attach_snap, follow_pos,
  // follow_rot, follow_both, snap, copy).
  void begin_target_pick_2d(const std::string& action);
  bool target_picking_2d() const { return !pick_action2d_.empty(); }
  bool finish_target_pick_2d(std::uint64_t target);
  void cancel_target_pick_2d() { pick_action2d_.clear(); }
  // Path of the v13 camera-migration backup made on open ("" = none).
  const std::string& camera_migration_backup_2d() const { return camera_backup2d_; }

  // --- Modal transforms + gizmo (Editor2DScreen_Transform.cpp) ---------------
  // G / R / S with the mouse over the viewport (Select tool), or a gizmo
  // drag; docs/transforms.md. px / py are canvas pixels (0, 0 = the
  // viewport's top-left). One undo step per confirm; cancel restores.
  bool modal_begin(XformOp op, float px, float py);
  bool modal_begin_handle(const GizmoHandle& h, float px, float py);
  void modal_mouse(float px, float py, bool ctrl = false, bool shift = false);
  void modal_axis(int axis, bool shift = false);
  bool modal_type(const std::string& chars);
  void modal_backspace();
  void modal_next_component();
  void modal_switch(XformOp op);
  bool modal_confirm();
  void modal_cancel();
  bool modal_active() const { return modal_.m.active(); }
  const ModalTransform& modal() const { return modal_.m; }
  std::string modal_header() const;
  // Alt+G / Alt+R / Alt+S on the selection.
  bool reset_transform(XformOp what);
  PivotMode pivot_mode() const { return pivot_mode_; }
  void set_pivot_mode(PivotMode p) { pivot_mode_ = p; }
  XformSpace orientation() const { return orientation_; }
  void set_orientation(XformSpace s) { orientation_ = s; }
  GizmoMode gizmo_mode() const { return gizmo_.mode; }
  void set_gizmo_mode(GizmoMode m) { gizmo_.mode = m; }
  // 2D cursor (Shift+RMB click; Shift+C back to 0, 0), world units.
  float cursor2d_x() const { return cursor2d_x_; }
  float cursor2d_y() const { return cursor2d_y_; }
  void set_cursor2d(float x, float y) { cursor2d_x_ = x; cursor2d_y_ = y; }
  // Where rotate / scale turn about for the current pivot mode (world).
  void pivot_point(float* wx, float* wy) const;
  // Gizmo handle under canvas pixel (px, py) (part None = nothing).
  GizmoHandle gizmo_pick(float px, float py) const;
  // Canvas pixels <-> world with the current camera and canvas size.
  void canvas_to_world(float px, float py, float* wx, float* wy) const;
  void world_to_canvas(float wx, float wy, float* px, float* py) const;

  // Scene Hierarchy filter (case-insensitive substring of the name).
  void set_hierarchy_filter(const std::string& text);
  bool hierarchy_row_visible(const Entity2D& e) const;

  // --- Tile tools (Editor2DScreen_Tiles.cpp) ---------------------------------
  // All public so --smoke drives the same paths as the viewport. Every
  // committed edit is one undo step and autosaves scene.json.
  TileTool tool() const { return tool_; }
  // Picking a paint tool with no TileMap selected selects the first one.
  void set_tool(TileTool tool);
  int brush_tile() const { return brush_tile_; }
  void set_brush_tile(int tile_id);
  int brush_size() const { return brush_size_; }
  void set_brush_size(int size);  // 1..3
  // Primary selection if it is a TileMap, else 0.
  std::uint64_t paint_target() const;
  // Paint / erase stroke: begin -> stroke_to_cell (any number) -> end is
  // ONE undo step ("Paint 12 tiles"). Cells outside the grid are clipped.
  bool begin_paint_stroke(std::uint64_t tilemap_id, bool erase);
  std::size_t stroke_to_cell(int col, int row);
  bool end_paint_stroke();
  void cancel_paint_stroke();  // Esc: restore the pre-stroke tiles
  bool stroke_active() const { return stroke_id_ != 0; }
  std::size_t fill_at(std::uint64_t tilemap_id, int col, int row);  // bucket
  std::size_t fill_rect_cells(std::uint64_t tilemap_id, int c0, int r0, int c1,
                              int r1, bool erase);
  bool eyedrop(std::uint64_t tilemap_id, int col, int row);
  bool resize_tilemap(std::uint64_t tilemap_id, int cols, int rows);
  bool set_tile_size(std::uint64_t tilemap_id, int tile_size);
  bool set_tileset(std::uint64_t tilemap_id, const std::string& rel_path);
  std::uint64_t create_tilemap();
  // Number of tiles the palette offers for a TileMap (tileset slices when
  // the image loads, else the built-in 16).
  int palette_count(const Entity2D& tilemap_entity);
  bool tileset_ready(const Entity2D& tilemap_entity);

  // --- Sprites / assets --------------------------------------------------------
  // "" removes the sprite. One undo step.
  bool assign_sprite(std::uint64_t id, const std::string& rel_path);
  // Sprite entity centred on a world point, sized to the image (64 px when
  // it cannot be read). One undo step.
  std::uint64_t create_sprite_at(const std::string& rel_path, float wx,
                                 float wy);
  // Copy an image into <project>/assets and refresh the list.
  bool import_asset(const std::string& source_path,
                    std::string* rel_out = nullptr);
  void refresh_assets();
  const std::vector<std::string>& asset_list() const { return assets_; }
  std::string asset_path(const std::string& rel) const;
  const TextureInfo& texture(const std::string& rel);
  SpriteState sprite_state(const Entity2D& e);

  // --- Play mode (Editor2DScreen_Play.cpp) -------------------------------------
  // Play copies the whole edit workspace aside and runs a runtime::World
  // built from it in the Viewport. Stop throws the world away and puts the
  // workspace back exactly (entities, selection, view, undo history).
  // Nothing done while playing writes scene.json or adds undo steps: edit
  // tools are locked and saving is refused.
  bool start_play();   // F5 / Ctrl+P
  void stop_play();    // F5 / Ctrl+P / Stop button (Esc does not stop)
  void toggle_play();
  void toggle_pause_play();  // F6
  bool step_play();          // F10 while paused: exactly one tick
  bool is_playing() const { return play_.active(); }  // playing or paused
  bool is_paused() const { return play_.paused(); }
  const runtime::PlaySession& play_session() const { return play_; }
  // Advance the running world by real time with this input. The viewport
  // calls it every frame with keyboard / gamepad input; --smoke calls it
  // with scripted input. Returns ticks run.
  int update_play(double real_seconds, const runtime::InputFrame& input);
  // Free (detached) editor camera while playing, instead of the follow cam.
  bool play_free_camera() const { return play_free_cam_; }
  void set_play_free_camera(bool on);
  // Save, then start ts_game --project <this project> as its own process.
  // False (with a status note) when ts_game is not built or will not start.
  bool launch_game();

  // --- Play with posse (Editor2DScreen_Posse.cpp) -----------------------------
  // A GameServer and 1-4 GameClients in this process (mp::LocalPosse) on a
  // loopback UDP port, riding a copy of the edit scene. P1 rides the
  // keyboard / pad, P2-P4 follow bot reins; the Viewport shows the chosen
  // rider's eyes (prediction + interpolation and all). The Network panel
  // holds the lag sim (latency / jitter / loss), the riders' RTT,
  // bandwidth and snapshot rate, and a trail map of server vs client.
  // Ordinary Play stops first; editing stays open (the posse rides a copy).
  bool start_posse();
  void stop_posse();
  bool posse_running() const { return posse_ && posse_->running(); }
  void toggle_posse();
  // Advance the posse by real time; P1 gets `p1`. draw_ui() calls it.
  void update_posse(double real_seconds, const runtime::PlayerInput& p1);
  mp::LocalPosse* posse() { return posse_.get(); }
  int posse_riders() const { return posse_riders_; }
  void set_posse_riders(int n);  // 1..4; live posse saddles / sends home
  const net::LinkSim& posse_sim() const { return posse_sim_; }
  void set_posse_sim(const net::LinkSim& sim);
  // Whose eyes the Viewport and trail map use (index into the clients).
  int posse_view() const { return posse_view_; }
  void set_posse_view(int index) { posse_view_ = index; }
  // Start a windowed ts_client riding into the running posse.
  bool open_posse_client_window();
  bool show_network_panel() const { return show_network_; }

  // --- Plugins (Editor2DScreen_Plugins.cpp) ----------------------------------
  // The project's plugins load on enter. Toggling saves project.json at
  // once; the loaded set changes when no ride is going (right away when
  // idle, else as Play / the posse stops). Plugin systems and Lua tables
  // join every Play ride.
  EditorPlugins& plugins() { return plugins_; }
  const EditorPlugins& plugins() const { return plugins_; }
  // set_enabled + apply when idle. False when the id is unknown.
  bool set_plugin_enabled(const std::string& id, bool on);
  // Apply a pending plugin change if nothing is riding. True if applied.
  bool apply_plugin_changes_if_idle();
  bool show_plugins_panel() const { return show_plugins_; }
  void set_show_plugins_panel(bool on) { show_plugins_ = on; }
  void set_show_network_panel(bool on) { show_network_ = on; }

  // --- Animation (Editor2DScreen_Anim.cpp) ------------------------------------
  // Animator component: add / edit / remove (nullopt) is one undo step and
  // autosaves scene.json, like every other component.
  bool set_animator(std::uint64_t id, std::optional<AnimatorData> animator,
                    const std::string& label = "Edit Animator");
  // Sets read from <project>/ for the Inspector, previews and the Stable.
  AnimLibrary& anim_library() { return anim_lib_; }
  const AnimLibrary::Entry* anim_entry(const std::string& set_rel);
  // Edit mode: animators play their default clip in the viewport (View >
  // Preview Animations). Off = they rest on its first frame.
  bool anim_preview() const { return anim_preview_; }
  void set_anim_preview(bool on) { anim_preview_ = on; }
  // Image for an entity in edit mode at `seconds` (animators sample their
  // default clip; plain sprites their source rect). False when nothing can
  // be drawn (no image, or it is missing).
  bool entity_image(const Entity2D& e, double seconds, EntityImage* out);
  // The .anim.json beside an image ("" when there is none on disk).
  std::string anim_set_for_image(const std::string& image_rel) const;

  // Stable: the animation-set panel. Edits save straight to the .anim.json
  // next to the sheet (no undo step; scene.json is not involved) and every
  // animator using the set picks them up.
  // Open a sheet image or a .anim.json. A sheet without a set starts a new,
  // unsaved one (one 32 px grid, no clips) that the first edit writes.
  bool stable_open(const std::string& rel);
  const std::string& stable_path() const { return stable_path_; }
  const AnimSet& stable_set() const { return stable_set_; }
  std::string stable_image() const;  // project-relative sheet path
  bool stable_saved() const { return stable_saved_; }
  // Normalize, write the .anim.json and refresh the library.
  bool stable_apply(AnimSet set, const std::string& note_text);
  bool stable_slice(int frame_w, int frame_h);  // fit cols / rows to the sheet
  int stable_add_clip(const std::string& name, int start, int count);  // index
  bool stable_rename_clip(int index, const std::string& name);
  bool stable_delete_clip(int index);
  bool stable_set_range(int index, int start, int count);
  bool stable_set_timing(int index, float fps, AnimMode mode);
  bool stable_set_default(const std::string& clip);
  int stable_clip() const { return stable_clip_; }
  void stable_select_clip(int index);

  // --- Collision (Editor2DScreen_Collision.cpp) -------------------------------
  // Tile solidity per tileset ("" = built-in palette). Each toggle is one
  // undo step ("Solid: Stone") and autosaves scene.json "tile_solidity".
  // Locked while playing, like every other edit.
  bool tile_solid(const std::string& tileset, int tile_id) const;
  bool set_tile_solid(const std::string& tileset, int tile_id, bool solid);
  bool toggle_tile_solid(const std::string& tileset, int tile_id);
  // Add / edit / remove (nullopt) an entity's collider: one undo step.
  bool set_collider(std::uint64_t id, std::optional<ColliderData> collider,
                    const std::string& label = "Edit Collider");
  // K: outline solid tiles and colliders in the viewport, edit and play.
  bool show_collision() const { return show_collision_; }
  void set_show_collision(bool on);
  void toggle_collision_overlay();
  // What the overlay draws inside a world rect: the edit scene, or the
  // running world while playing (triggers with a rider inside are active).
  void build_collision_overlay(const runtime::WorldRect& view,
                               std::vector<runtime::OverlayBox>* out) const;
  // "Player rode into Gate (tick 26)" while playing; "" otherwise.
  std::string last_trigger_text() const;

  // --- Scripts (Editor2DScreen_Scripts.cpp) ------------------------------------
  // Script component: set / clear (nullopt) is one undo step + autosave.
  bool set_script(std::uint64_t id, std::optional<ScriptData> script,
                  const std::string& label = "Edit Script");
  // One property override (nullopt = back to the file's default): one undo
  // step + autosave.
  bool set_script_prop(std::uint64_t id, const std::string& name,
                       std::optional<ScriptValue> value);
  // The whole script stack (Inspector order = run order): one undo step.
  bool set_scripts(std::uint64_t id, std::vector<ScriptData> stack,
                   const std::string& label = "Edit Scripts");
  // Adds rel at the bottom of the stack (Add Component, drag and drop
  // onto the viewport / Hierarchy / Inspector). False when playing, the
  // entity is a tile map, it already runs that file, or the stack is full.
  bool attach_script(std::uint64_t id, const std::string& rel);
  // New Script: scripts/<stem><ext> from a template, then attached to id
  // (0 = just write the file). rel_out gets the new file.
  bool new_script(std::uint64_t id, const std::string& name, ScriptLang lang,
                  const std::string& template_id, std::string* rel_out = nullptr);
  // Opens the Inspector's Add Component menu next frame (docs screenshot).
  void open_add_component_menu() { add_component_open_ = true; }
  // <project>/scripts/**.lua, project-relative and sorted.
  void refresh_scripts();
  const std::vector<std::string>& script_list() const { return scripts_; }
  // scripts/<name>.lua from a template (sample_scripts::templates()); never
  // overwrites. rel_out gets the project-relative path.
  bool create_script(const std::string& name, const std::string& template_id,
                     std::string* rel_out = nullptr);
  // Hand a script to the OS default editor (ShellExecute / open / xdg-open).
  bool open_script(const std::string& rel);
  // Declared props of a script, re-read when the file changes.
  const ScriptInfo& script_info(const std::string& rel);
  // Play: re-run scripts saved since the last check (the viewport polls
  // twice a second). Returns files reloaded.
  int reload_play_scripts();

  // --- Telegraph console (Editor2DScreen_Console.cpp) ------------------------
  // World lines (scripts, toasts, triggers) arrive while playing; editor
  // notes always. Clicking a line with an entity selects it.
  TelegraphLog& telegraph() { return telegraph_; }
  const TelegraphLog& telegraph() const { return telegraph_; }
  // Move the running world's new lines into the Telegraph.
  std::size_t pump_play_logs();
  // Select the entity a line points at (and frame it when editing). False
  // when there is none, or it only lived in the ride.
  bool focus_log_entry(const runtime::LogEntry& entry);

 private:
  void draw_attach_menu_2d();
  void draw_inspector_follow_2d(std::uint64_t id);
  void draw_object_menu_2d();
  bool attach_target_list_2d(const char* id, std::uint64_t* out);
  void migrate_cameras_on_open_2d(const std::vector<std::string>& lines);
  std::string pick_action2d_;
  std::string camera_backup2d_;
  bool copy_pos2d_ = true, copy_rot2d_ = true, copy_scale2d_ = false;
  friend class PossePlugin;  // registers the posse UI as plugin contributions
  void draw_ui();
  void draw_menu_bar();
  void draw_menus();  // menu bar contents (also harvested for the palette)
  void setup_field_guide();
  void draw_context_menus_for_audit();
  ux::EditorUx ux_{false};
  bool settings_requested_ = false;
  void draw_toolbar();
  void draw_status_bar();
  void draw_hierarchy();
  void draw_viewport();
  void draw_inspector();
  void draw_inspector_components(Entity2D& e);
  // Player / Camera2D / SpawnPoint sections + "Add component".
  void draw_inspector_gameplay(std::uint64_t id);
  void draw_play_controls();   // toolbar Play / Pause / Step / Launch
  void draw_play_viewport();   // Viewport2D while playing
  void draw_play_status();     // status-bar section while playing
  void draw_play_menu();
  // Script toasts over the play viewport, newest at the bottom.
  void draw_play_toasts(ImDrawList* draw, const ImVec2& canvas_pos,
                        const ImVec2& canvas_size);
  // Script section of the Inspector; Scripts panel; Telegraph panel and its
  // status-bar badge.
  void draw_inspector_script(std::uint64_t id);
  // "Add Component" at the foot of the Inspector (components + scripts).
  void draw_inspector_add_component(std::uint64_t id);
  void draw_inspector_audio(std::uint64_t id);
  void draw_inspector_fx(std::uint64_t id);
  void draw_lighting_panel();
  bool show_lighting_ = false;
  void draw_wagon_sounds();
  bool set_audio_source(std::uint64_t id, const std::optional<AudioSourceData>& audio,
                        const char* label);
  std::vector<std::string> sounds_;  // project sounds under assets/
  void draw_scripts_panel();
  void draw_console();
  void draw_telegraph_badge();
  // Animator section of the Inspector and the Stable panel
  // (Editor2DScreen_Anim.cpp / Editor2DScreen_Stable.cpp).
  void draw_inspector_animator(std::uint64_t id);
  void draw_stable();
  // Collision section of the Inspector (Editor2DScreen_Collision.cpp).
  void draw_inspector_collider(std::uint64_t id);
  void draw_inspector_body(std::uint64_t id);  // Rigid Body + Joint (Box2D)
  // Tile Palette "Solid" checkbox for the brush tile.
  void draw_tile_solid_controls(const Entity2D* target);
  // K toggles the overlay (both viewports call this).
  void handle_collision_hotkey();
  // Overlay on a canvas whose centre shows world (center_x, center_y).
  void draw_collision_overlay(ImDrawList* draw, const ImVec2& canvas_pos,
                              const ImVec2& canvas_size, float center_x,
                              float center_y, float zoom);
  // Keyboard (WASD / arrows, E / Space, Shift, Enter) + gamepads via GLFW.
  runtime::InputFrame poll_play_input() const;
  void draw_tile_palette();
  void draw_supply_wagon();
  // Shared asset picker combo; returns true + rel_out when a choice is made
  // ("" = none).
  bool asset_combo(const char* label, const std::string& current,
                   const char* none_label, std::string* rel_out);
  // Draws a palette tile (built-in colour or tileset slice) into a rect.
  void draw_tile_swatch(ImDrawList* draw, const Entity2D* tilemap_entity,
                        int tile_id, const ImVec2& p0, const ImVec2& p1);
  void draw_tool_buttons(float button_width = 50.0f, int per_row = 6);
  void handle_hotkeys();
  void draw_help_menu_contents();
  // Inspector float field that live-snaps to the grid when snap is on.
  // slot identifies the field so the raw (unsnapped) drag value survives
  // between frames. Returns true when the value changed this frame.
  bool transform_field(const char* label, float* field, int slot, float vmin,
                       float vmax, bool is_extent);
  void note(std::string message);  // transient status-bar message
  // Inspector widget hook: call right after the widget. Opens the coalesced
  // edit on activation and commits it on deactivation.
  void track_inspector_item(const char* label);
  // Discrete edit helper: flush any coalesced edit, then snapshot.
  Workspace2D::Snapshot prepare_edit();
  double now_seconds() const;
  void setup_default_dock_layout(unsigned int dockspace_id);

  // Right-click menu body shared by the viewport and the Hierarchy.
  void draw_entity_context_menu(std::uint64_t clicked);
  void draw_hierarchy_row(const Entity2D& e, int depth);
  void begin_rename(std::uint64_t id);
  void commit_rename();
  void cancel_rename();

  void mark_dirty();
  void draw_prefabs_panel();
  void draw_prefab_inspector(std::uint64_t id);
  bool refresh_prefab_overrides();
  bool prefab_set_entities(const std::vector<json::Value>& js, const std::string& label,
                           Workspace2D::Snapshot before);
  void prefab_sync_all_on_load();
  PrefabLibrary prefabs_;
  AssetBrowser asset_browser_;
  bool show_asset_browser_ = true;
  void open_asset_browser();
  // Pull a Vault item dropped on the viewport, then place it there.
  void drop_vault_item(const std::string& item, float wx, float wy);
  // The Vault (asset library shared by every claim).
  VaultPanel vault_panel_;
  bool show_vault_ = true;
  bool show_prefabs_ = true;
  char prefab_name_[64] = "Prefab";
  double prefab_poll_time_ = 0.0;
  void mark_dirty_and_autosave();
  // Discrete edit: commit + autosave + note when something changed.
  bool commit_discrete(Workspace2D::Snapshot before, const std::string& label,
                       const std::string& note_text);

  ProjectInfo project_;
  Workspace2D workspace_;
  std::string scene_path_;
  bool dirty_ = false;
  std::string last_save_error_;
  // scene.json on disk is from a newer TombStone: shown an error and never
  // saved over (cleared on the next on_enter).
  bool scene_locked_ = false;
  std::string scene_locked_why_;
  // Telegraph line + readable error modal for a failed save.
  void report_save_failure(const std::string& err);
  bool quit_requested_ = false;
  bool back_requested_ = false;

  // Panel visibility (View menu).
  bool show_hierarchy_ = true;
  bool show_viewport_ = true;
  bool show_inspector_ = true;
  bool show_status_bar_ = true;
  bool show_toolbar_ = true;
  bool show_tile_palette_ = true;
  bool show_supply_wagon_ = true;
  bool show_stable_ = true;
  bool show_ui_preview_ = false;
  UiPreview ui_preview_;
  bool show_scripts_ = true;
  bool show_console_ = true;
  bool show_mixer_ = false;
  bool focus_console_ = false;  // bring the Telegraph forward next frame
  bool dock_layout_initialized_ = false;
  bool dock_reset_requested_ = false;  // Window > Layouts > Reset layout

  // Hierarchy rename state.
  bool renaming_ = false;
  std::uint64_t rename_id_ = 0;
  char rename_buf_[128]{};

  // Viewport interaction.
  enum class DragMode { None, Move, Marquee, Paint, RectFill, Handle };
  bool panning_ = false;
  DragMode drag_mode_ = DragMode::None;
  std::uint64_t drag_hit_id_ = 0;  // entity under the cursor at press (0=none)
  bool drag_additive_ = false;     // Ctrl/Shift held at press
  bool drag_moved_ = false;        // passed the drag threshold
  bool viewport_focused_ = false;
  bool hierarchy_focused_ = false;
  bool nudge_pending_save_ = false;  // autosave once arrows are released
  // Editor tools.
  bool dup_drag_ = false;           // the open drag is an Alt+drag copy
  int handle_x_ = 0, handle_y_ = 0;  // resize handle side
  bool handle_rotate_ = false;
  bool handle_active_ = false;
  float handle_start_wx_ = 0.0f, handle_start_wy_ = 0.0f;
  float viewport_w_ = 800.0f, viewport_h_ = 600.0f;  // last canvas size
  // Modal transforms + gizmo (Editor2DScreen_Transform.cpp).
  struct ModalState {
    ModalTransform m;
    std::size_t active = 0;        // index into the workspace's root starts
    float pivot_x = 0, pivot_y = 0;  // world
    float mx = 0, my = 0;          // last mouse (canvas px)
    bool snap = false;
    bool from_gizmo = false;
    float shown[2] = {0, 0};       // header values
    Workspace2D::Snapshot before;  // the undo step's before-state
  };
  ModalState modal_;
  Gizmo2D gizmo_;
  PivotMode pivot_mode_ = PivotMode::Median;
  XformSpace orientation_ = XformSpace::Global;
  float cursor2d_x_ = 0.0f, cursor2d_y_ = 0.0f;
  GizmoHandle gizmo_hover_;
  bool viewport_hovered_ = false;
  float viewport_x_ = 0.0f, viewport_y_ = 0.0f;  // canvas corner on screen
  bool open_pivot_menu_ = false;
  bool swallow_rmb_ = false;  // the RMB that cancelled: no context menu
  void modal_apply();
  void handle_modal_keys();
  // Header widgets (gizmo, orientation, pivot) or the modal line.
  void draw_transform_header();
  // Cursor, gizmo / modal guides; x0 / y0 = canvas corner on screen.
  void draw_gizmo_overlay(ImDrawList* draw, float x0, float y0);
  // Gizmo centre (canvas px) and its move / scale axes (degrees).
  void gizmo_frame(float* cx, float* cy, float* axes_deg, float* scale_deg) const;
  bool rmb_dragged_ = false;         // RMB press panned (no context menu)
  std::uint64_t context_id_ = 0;     // entity the context menu opened on
  // Where the viewport's right-click landed (Create here / cursor here).
  bool context_in_viewport_ = false;
  float context_world_x_ = 0.0f, context_world_y_ = 0.0f;
  // Right-click > Copy components / Inspector header > Copy values.
  std::optional<Entity2D> comp_clip_;
  std::string comp_clip_name_;  // "" = every component (Copy components)
  void paste_components_2d(const std::string& only);
  // Inspector component headers: right-click for reset / copy / paste
  // values / remove / docs. The _header_ form draws a CollapsingHeader too.
  void component_header_menu_2d(std::uint64_t id, const char* name);
  bool component_header_2d(std::uint64_t id, const char* name);
  std::function<void()> header_op_;  // run after the Inspector draws
  char hierarchy_filter_[96]{};
  std::uint64_t hier_drag_id_ = 0;
  // Hierarchy rows in drawn (tree) order this frame: Shift+click ranges.
  std::vector<std::uint64_t> hier_rows_;
  std::vector<std::uint64_t> hier_rows_prev_;  // last frame's (full) order
  // Hierarchy actions are queued while rows draw and applied after the loop
  // (the entity list must not change under the iteration).
  bool hier_open_context_ = false;
  std::uint64_t hier_drop_id_ = 0, hier_drop_target_ = 0;
  Workspace2D::HierDrop hier_drop_where_ = Workspace2D::HierDrop::After;
  bool hier_drop_root_ = false;
  std::uint64_t hier_toggle_lock_ = 0, hier_toggle_hide_ = 0;

  // Which coalesced edit (if any) currently owns workspace_.edit_open().
  enum class EditSource { None, Drag, Inspector, Nudge, Paint };
  EditSource edit_source_ = EditSource::None;
  double last_nudge_time_ = 0.0;
  std::vector<std::uint64_t> nudge_selection_;  // who the open nudge moves
  // Primary entity as it was at the top of this frame's Inspector, so an
  // edit applied on the activation frame (color picker click) is undoable.
  Entity2D insp_frame_entity_{};
  bool insp_frame_valid_ = false;

  // Hierarchy shift-click range anchor (index into entities()).
  std::size_t range_anchor_ = 0;
  bool has_range_anchor_ = false;

  // Inspector live-snap drag state.
  int insp_drag_slot_ = -1;
  std::uint64_t insp_drag_entity_ = 0;
  float insp_drag_raw_ = 0.0f;

  // Tile tools.
  TileTool tool_ = TileTool::Select;
  int brush_tile_ = 1;
  int brush_size_ = 1;
  std::uint64_t stroke_id_ = 0;
  bool stroke_erase_ = false;
  bool stroke_has_last_ = false;
  int stroke_last_col_ = 0;
  int stroke_last_row_ = 0;
  std::size_t stroke_cells_ = 0;
  int rect_c0_ = 0;
  int rect_r0_ = 0;
  // Hover readout for the status bar.
  bool hover_cell_valid_ = false;
  int hover_col_ = 0;
  int hover_row_ = 0;
  int hover_tile_ = 0;

  // Assets (<project>/assets) + texture cache (GL handles in the admin app,
  // size-only when headless).
  std::vector<std::string> assets_;
  double assets_refresh_time_ = -1000.0;
  TextureCache textures_;
  FolderBrowser import_browser_;
  std::string wagon_selected_;  // highlighted asset in the Supply Wagon

  // Play mode.
  runtime::PlaySession play_;
  // Sound for Play and the posse (one at a time; null when idle).
  std::unique_ptr<audio::SoundStage> sound_stage_;
  void begin_sound_ride();
  void end_sound_ride();
  std::optional<Workspace2D> play_backup_;  // the edit workspace, untouched
  bool play_free_cam_ = false;
  bool play_focus_viewport_ = false;  // focus Viewport2D on the next frame
  float play_cam_x_ = 0.0f;
  float play_cam_y_ = 0.0f;
  float play_cam_zoom_ = 1.0f;
  // The mouse over the Play canvas in world px (top-down "face mouse").
  bool play_aim_ = false;
  float play_aim_x_ = 0.0f;
  float play_aim_y_ = 0.0f;
  std::vector<runtime::DrawQuad> play_quads_;
  PlayUi play_ui_;  // the project's ui/ documents over the Play view
  profiler::DebugTools debug_tools_;  // ts.profiler overlay (F3) + console (backtick)
  bool debug_tools_on_ = false;
  ConsoleLineState telegraph_console_;

  // Animation: sets for previews / the Stable, and the Stable's open set.
  AnimLibrary anim_lib_;
  bool anim_preview_ = true;
  std::string stable_path_;   // project-relative .anim.json ("" = none)
  AnimSet stable_set_;
  bool stable_saved_ = false;  // the file exists on disk
  int stable_clip_ = -1;
  int stable_anchor_ = -1;     // thumbnail range pick: first cell clicked
  double stable_play_start_ = 0.0;
  bool stable_playing_ = true;
  char stable_name_buf_[64]{};
  int stable_frame_w_ = 32;
  int stable_frame_h_ = 32;

  // Collision overlay (K).
  bool show_collision_ = false;
  std::vector<runtime::OverlayBox> overlay_boxes_;

  // Scripts: the project's .lua files, what each declares, and the panel.
  std::vector<std::string> scripts_;
  double scripts_refresh_time_ = -1000.0;
  runtime::ScriptLibrary script_lib_;
  std::map<std::string, ScriptInfo> script_infos_;
  std::string scripts_selected_;  // highlighted file in the Scripts panel
  char new_script_name_[64]{};
  int new_script_template_ = 0;
  int new_script_lang_ = 0;
  AddScriptMenu add_script_menu_;
  bool add_component_open_ = false;
  std::uint64_t script_drop_id_ = 0;  // hierarchy row a script was dropped on
  std::string script_drop_rel_;
  double play_scripts_poll_ = 0.0;  // last hot-reload check while playing

  // Telegraph console.
  TelegraphLog telegraph_;

  // Plugins.
  void open_plugins();
  void draw_plugins_window();
  void draw_plugin_panels();
  void draw_plugin_view_items();
  // Contributions to `menu` ("Play", "Toolbar", or a top-level menu).
  void draw_plugin_menu_items(const std::string& menu);
  void draw_plugin_top_menus();
  PluginsPanelState plugins_panel_;
  bool show_plugins_ = true;
  bool plugins_applying_ = false;  // no re-entry from plugin unload hooks
  std::map<std::string, bool> plugin_panel_open_;

  // Play with posse.
  void draw_network_panel();
  void draw_posse_menu_items();
  void draw_posse_toolbar_button();
  void draw_posse_viewport();
  void draw_posse_map(float height);
  void pump_posse_logs();
  std::unique_ptr<mp::LocalPosse> posse_;
  int posse_riders_ = 2;
  net::LinkSim posse_sim_{60, 15, 0.02f};
  bool posse_bots_ = true;
  int posse_view_ = 0;
  int posse_windows_ = 0;
  bool show_network_ = true;
  bool focus_network_ = false;
  std::vector<runtime::DrawQuad> posse_quads_;
  char telegraph_search_[128]{};
  bool telegraph_autoscroll_ = true;
  std::uint64_t telegraph_seen_total_ = 0;  // for auto-scroll on new lines

  // Status-bar note (e.g. "Duplicated 3").
  std::string status_note_;
  double status_note_time_ = -1000.0;
  // Last member: destroyed first, so plugins unhitch while the rest of the
  // screen (the posse they may stop) is still alive.
  EditorPlugins plugins_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
