#pragma once
#include <map>
#include "debugtools/DebugTools.h"
#include "editor/plugins/ProfilerPanels.h"

// The 3D editor ("dimension": "3d" claims). Docked panels in the Dusk
// theme:
//   Viewport3D  GL 3.3 render of the scene (offscreen target shown as an
//               image): orbit (RMB / Alt+LMB), pan (MMB), zoom (wheel), fly
//               (hold RMB + WASD/QE), F frames the selection, click picks,
//               Blender-style gizmos (Q / W / E / T / Y) and modal
//               G / R / S transforms with pivots, orientation and snap
//               (Editor3DScreen_Transform.cpp, docs/transforms.md).
//   Hierarchy   entities; click / Ctrl+click selection, create menu.
//   Inspector   transform (position, rotation in degrees, scale) and the
//               components: mesh, material, light, collider, player, script.
//   Assets      primitives plus the project's .gltf / .glb models and
//               images; drag a model into the viewport to place it, an
//               image onto an entity to make it the albedo texture.
//   Environment sky, sun, shadows, ambient.
//   Telegraph   editor + play log.   Plugins: the Outfitter's Wagon.
// Tools (Editor3DScreen_Tools.cpp): parenting, clipboard, group, lock /
// hide, align, drop to ground, Alt+drag copies, context menus.
// Every edit is one undo step (Ctrl+Z / Ctrl+Y) and autosaves scene.json
// (v9). Play (F5) rides the scene in a World3D copy; Stop restores it.
// VR claims ("dimension": "vr", Editor3DScreen_VR.cpp): Play runs the
// scene in VR mode through the desktop simulator, "Preview in headset"
// through OpenXR (falls back to the simulator), with a Headset Mirror
// window showing both eyes; inspector has the VR Rig / Grabbable
// components.

#include "audio/SoundStage.h"
#include "audio/SoundStage3D.h"
#include "editor/AppState.h"
#include "editor/assets/AssetBrowser.h"
#include "editor/assets/PrefabLibrary.h"
#include "editor/vault/VaultPanel.h"
#include "editor/ProjectInfo.h"
#include "editor/editor3d/Gizmo3D.h"
#include "editor/transform/ModalTransform.h"
#include "editor/plugins/EditorPlugins.h"
#include "editor/plugins/PluginsPanel.h"
#include "editor/scripting/ScriptComponents.h"
#include "editor/screens/IScreen.h"
#include "editor/ui/PlayUi.h"
#include "render3d/Renderer3D.h"
#include "render3d/Attach3D.h"
#include "render3d/Scene3D.h"
#include "render3d/SceneView.h"
#include "terrain/Terrain.h"
#include "terrain/TerrainComponent.h"
#include "runtime/Log.h"
#include "world3d/World3D.h"
#include "xr/VrPresenter.h"
#include "xr/XrSession.h"
#include "editor/ux/EditorUx.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

class Editor3DScreen final : public IScreen {
 public:
  explicit Editor3DScreen(ProjectInfo project);
  ~Editor3DScreen() override;

  AppState state() const override { return AppState::Editor3D; }
  void on_enter() override;
  void on_exit() override;
  AppState on_update(float delta_seconds) override;

  void request_back_to_projects() { back_requested_ = true; }
  // Field Guide: action registry + command palette (Ctrl+Shift+P).
  ux::EditorUx& field_guide() { return ux_; }
  void request_quit() { quit_requested_ = true; }
  const ProjectInfo& project() const { return project_; }
  EditorPlugins& plugins() { return plugins_; }
  std::map<std::string, bool>& plugin_panel_open() { return plugin_panel_open_; }

  // ---- Scene + selection --------------------------------------------------
  const scene3d::Scene3D& scene() const { return scene_; }
  const scene3d::Entity3D* find(std::uint64_t id) const { return scene_.find(id); }
  const std::vector<std::uint64_t>& selection() const { return scene_.selection; }
  std::uint64_t primary() const;
  void select(std::uint64_t id, bool additive = false);
  void set_show_physics_settings(bool on) { show_physics_ = on; }

  // ---- Terrain + foliage (Editor3DScreen_Terrain.cpp) ----
  struct TerrainTool {
    bool active = false;  // brushes on: the left button sculpts / paints in the viewport
    int mode = 0;         // 0 sculpt, 1 paint layers, 2 foliage
    terrain::BrushOp op = terrain::BrushOp::Raise;
    float radius = 6.0f;
    float strength = 6.0f;
    float hardness = 0.4f;
    float noise_scale = 4.0f;
    int layer = 1;
    int foliage_type = 0;
    bool erase = false;
    float fill = 0.6f;
  };
  TerrainTool& terrain_tool() { return terrain_tool_; }
  void set_show_terrain(bool on) { show_terrain_ = on; }
  // New terrain entity (res 2^n+1, size metres); look: meadow / desert / snowy.
  std::uint64_t create_terrain(int resolution, float size, bool hills, const std::string& look);
  // The selected terrain, else the first terrain in the scene (0 = none).
  std::uint64_t terrain_target() const;
  // Closest terrain hit along a world ray.
  bool terrain_ray(const render3d::Ray& ray, float* t, std::uint64_t* id) const;
  // Brush strokes (one undo step each). `world` is a point on the terrain.
  bool terrain_stroke_begin(std::uint64_t id);
  bool terrain_stroke(const render3d::Vec3& world, float dt, bool invert = false);
  void terrain_stroke_end();
  bool terrain_stroking() const { return stroke_.has_value(); }
  bool save_terrains(std::string* error);
  void clear_selection();

  // ---- Edits (each one undo step + autosave) ------------------------------
  // "cube" / "plane" / "sphere" / "capsule".
  std::uint64_t create_primitive(const std::string& shape, const render3d::Vec3& pos);
  std::uint64_t create_light(bool directional, const render3d::Vec3& pos);
  std::uint64_t create_empty(const render3d::Vec3& pos);
  // Drop an Assets entry: a model (.gltf / .glb or "primitive:...") makes
  // an entity at pos; an image onto `target` sets its albedo texture.
  // Model colliders fitted at import (Collider3D::fit) follow their model:
  // refit to the current bounds. `migrate` also refits boxes that were
  // sized from the pre-fix raw-vertex bounds (scenes from before the
  // bounds fix). One undo step; returns how many changed.
  int refit_model_colliders(bool migrate);
  // Inspector "Fit to mesh": fit one entity's collider with `fit`
  // ("auto" / "box" / "capsule" / "convex" / "mesh"). False = no model.
  bool fit_collider_to_mesh(std::uint64_t id, const std::string& fit);
  bool show_hitboxes() const { return show_hitboxes_; }
  void set_show_hitboxes(bool on) { show_hitboxes_ = on; }
  std::uint64_t drop_asset(const std::string& rel, const render3d::Vec3& pos,
                           std::uint64_t target = 0);
  std::size_t duplicate_selected();
  std::size_t delete_selected();
  bool set_transform(std::uint64_t id, const render3d::Transform& t,
                     const std::string& label = "Transform");
  // Replace one entity wholesale (Inspector edits).
  bool replace_entity(const scene3d::Entity3D& e, const std::string& label);
  bool set_environment(const render3d::Environment& env, const std::string& label);
  bool rename(std::uint64_t id, const std::string& name);

  bool undo();
  bool redo();
  bool can_undo() const { return !undo_.empty(); }
  bool can_redo() const { return !redo_.empty(); }
  const std::string& undo_label() const;
  const std::string& redo_label() const;
  std::size_t undo_depth() const { return undo_.size(); }

  bool save_scene(std::string* error = nullptr);
  bool reload_scene(std::string* error = nullptr);

  // ---- Viewport (pixels in a vw x vh view) ---------------------------------
  render3d::Camera3D editor_camera() const;
  // Screen position and size of the view drawn last frame.
  void viewport_screen_rect(float* x, float* y, float* w, float* h) const {
    *x = viewport_x_;
    *y = viewport_y_;
    *w = viewport_w_;
    *h = viewport_h_;
  }
  void orbit(float dyaw_deg, float dpitch_deg);
  void dolly(float factor);
  void pan(float dx_px, float dy_px, float vh);
  void frame_selection();
  // Entity under the pixel (0 = none).
  std::uint64_t pick(float px, float py, float vw, float vh);
  // Ground point (y = 0) under the pixel; false when looking at the sky.
  bool ground_point(float px, float py, float vw, float vh, render3d::Vec3* out) const;

  Gizmo3D& gizmo() { return gizmo_; }
  GizmoSnap snap_steps() const;
  bool snapping() const { return scene_.snap; }
  void set_snapping(bool on);
  // Gizmo drag on the selection: begin (false on a miss), move, end (one
  // undo step). Same code the mouse drives; a drag runs as a modal
  // transform with the handle's constraint (Editor3DScreen_Transform.cpp).
  bool gizmo_begin(float px, float py, float vw, float vh);
  bool gizmo_begin_axis(int axis, float px, float py, float vw, float vh);
  void gizmo_move(float px, float py, float vw, float vh);
  void gizmo_end();
  void gizmo_cancel();
  // Handle under the mouse for the current selection (part None = miss).
  GizmoHandle gizmo_pick(float px, float py, float vw, float vh) const;

  // ---- Modal transforms (Editor3DScreen_Transform.cpp) ----------------------
  // G / R / S: move / rotate / scale the selection roots (children ride
  // along) from the mouse at (px, py). modal_mouse each frame (ctrl flips
  // snapping, shift = precision); axis keys, typed numbers; confirm = one
  // undo step, cancel restores. See docs/transforms.md.
  bool modal_begin(XformOp op, float px, float py, float vw, float vh);
  bool modal_begin_handle(const GizmoHandle& h, float px, float py, float vw, float vh);
  void modal_mouse(float px, float py, bool ctrl = false, bool shift = false);
  void modal_axis(int axis, bool shift = false);
  bool modal_type(const std::string& chars);  // "2", "-90", "0.5"
  void modal_backspace();
  void modal_next_component();
  void modal_trackball();
  void modal_switch(XformOp op);
  bool modal_confirm();
  void modal_cancel();
  bool modal_active() const { return modal_.m.active(); }
  const ModalTransform& modal() const { return modal_.m; }
  // Header line while a transform runs ("Move  Dx 2.000 ...").
  std::string modal_header() const;
  // Alt+G / Alt+R / Alt+S: clear location / rotation / scale of the
  // selection roots (world values; one undo step).
  bool reset_transform(XformOp what);
  PivotMode pivot_mode() const { return pivot_mode_; }
  void set_pivot_mode(PivotMode p) { pivot_mode_ = p; }
  XformSpace orientation() const { return orientation_; }
  void set_orientation(XformSpace s) { orientation_ = s; }
  const render3d::Vec3& cursor3d() const { return cursor3d_; }
  void set_cursor3d(const render3d::Vec3& p) { cursor3d_ = p; }
  // Where rotate / scale turn about for the current selection.
  render3d::Vec3 pivot_point() const;
  GizmoFrame gizmo_frame() const;
  // Snap targets for moves: grid increments or the surface under the mouse.
  bool snap_to_surface() const { return snap_surface_; }
  void set_snap_to_surface(bool on) { snap_surface_ = on; }

  // One frame of the scene (editor or play view) through the renderer
  // (GL offscreen when a context exists, else the null backend).
  render3d::FrameStats render_frame(int width, int height);
  const char* renderer_backend() const;
  std::uint64_t viewport_texture() const;

  // ---- Play ----------------------------------------------------------------
  bool play_start();
  void play_stop();
  bool playing() const { return world_.running(); }
  world3d::World3D& world() { return world_; }
  void play_ticks(int n, const world3d::Input3D& in) { world_.run_ticks(n, in); }

  // ---- VR (Editor3DScreen_VR.cpp) -------------------------------------------
  bool vr_project() const { return vr_enabled(project_); }
  // Start the scene in VR mode: OpenXR headset unless `simulator`, falling
  // back to the simulator (the reason goes to the Telegraph).
  bool vr_preview_start(bool simulator);
  void vr_preview_stop();
  bool vr_previewing() const { return vr_session_ != nullptr; }
  xr::XrSession* vr_session() { return vr_session_.get(); }
  xr::SimXrSession* vr_sim();
  const xr::VrFrameResult& vr_last_frame() const { return vr_frame_; }
  // One preview frame (on_update calls it; tests may too).
  void vr_preview_frame(double dt);
  const std::string& vr_status() const { return vr_status_; }

  // ---- Tools (Editor3DScreen_Tools.cpp) ------------------------------------
  // Each edit is one undo step + autosave; all refuse while playing.
  // Selection roots: selected entities without a selected ancestor.
  std::vector<std::uint64_t> selection_roots() const;
  std::vector<std::uint64_t> children_of(std::uint64_t id) const;
  bool effectively_hidden(const scene3d::Entity3D& e) const;  // self or an ancestor
  bool effectively_locked(const scene3d::Entity3D& e) const;
  // World bounds of one entity (unit box when the renderer is not up).
  render3d::Aabb world_bounds(const scene3d::Entity3D& e);
  // Clipboard: the selection's subtrees as 3D scene.json text. Paste works
  // across scenes / projects (ids remapped, roots land at the scene root).
  std::string copy_json() const;
  std::size_t paste_json(const std::string& text, bool offset);
  bool copy_selected();
  std::size_t cut_selected();
  std::size_t paste();
  static const std::string& clipboard_text();
  // Parenting. where: 0 before, 1 after, 2 inside `target`.
  bool set_parent(std::uint64_t id, std::uint64_t parent);
  bool hierarchy_drop(std::uint64_t id, std::uint64_t target, int where);
  std::uint64_t group_selected();    // Ctrl+G: empty "Group" at the centre
  std::size_t ungroup_selected();    // Ctrl+Shift+G
  bool set_locked(std::uint64_t id, bool on);
  bool set_hidden(std::uint64_t id, bool on);  // editor only (editor_hidden)
  bool lock_selected(bool on);       // Ctrl+L / Ctrl+Shift+L
  bool hide_selected(bool on);       // H / Shift+H
  // Align selection roots on axis 0/1/2 (x/y/z) at -1 min, 0 centre, 1 max
  // of the selection's bounds; distribute spaces centres evenly (3+).
  bool align_selected(int axis, int where);
  bool distribute_selected(int axis);
  // Drop to ground (End): each root falls until its bounds rest on the
  // surface below (another entity or the y = 0 ground).
  std::size_t drop_to_ground();
  // Nearest surface under the pixel (entities outside `skip` + ground).
  bool surface_point(float px, float py, float vw, float vh,
                     const std::vector<std::uint64_t>& skip, render3d::Vec3* out);
  // Alt+drag on the gizmo: copy the selection, then drag the copies (one
  // "Duplicate" undo step; Esc removes the copies).
  bool gizmo_begin_duplicate(float px, float py, float vw, float vh);
  // Moves stick to surfaces under the mouse while on (also: Snap target
  // Surface with snapping on).
  void set_surface_snap(bool on) { surface_snap_ = on; }

  // ---- Scripts (Editor3DScreen_Scripts.cpp) ---------------------------------
  // The entity's script stack (Unity-style components: several per entity,
  // run top to bottom). Each edit is one undo step; all refuse while playing.
  bool set_scripts(std::uint64_t id, std::vector<ScriptData> stack,
                   const std::string& label = "Edit Scripts");
  // Append `rel` (refuses duplicates and the 16-script cap).
  bool attach_script(std::uint64_t id, const std::string& rel);
  // Write scripts/<name>.<ext> from a template; attach it when id != 0.
  bool new_script(std::uint64_t id, const std::string& name, ScriptLang lang,
                  const std::string& template_id, std::string* rel_out = nullptr);
  const std::vector<std::string>& scripts() const { return scripts_; }
  void open_add_component_menu() { add_component_open_ = true; }

  // ---- Cameras (Editor3DScreen_Camera.cpp, docs/camera.md) ------------------
  // Create > Camera: a Fixed camera where the editor view is, looking the
  // same way. One undo step; selected.
  std::uint64_t create_camera();
  // Replace an entity's camera settings (one undo step).
  bool set_camera(std::uint64_t id, const scene3d::PlayCamera3D& c,
                  const std::string& label = "Edit Camera");
  // "Make active camera": on, priority above every other camera.
  bool make_active_camera(std::uint64_t id);
  // The camera Play would start with (highest-priority active; 0 = none).
  std::uint64_t play_camera_id() const;
  // Ctrl+Alt+Numpad0: move the camera (selected, else the play camera) to
  // the editor view. One undo step.
  bool align_camera_to_view(std::uint64_t id = 0);
  // Numpad0: look through a camera (0 = selected / play camera); again (or
  // orbiting / panning / zooming) goes back to the editor view.
  bool look_through_camera(std::uint64_t id = 0);
  void stop_looking_through() { look_through_ = 0; }
  std::uint64_t looking_through() const { return look_through_; }
  // The view a camera entity gives (projection, clip planes, transform).
  render3d::Camera3D camera_view(const scene3d::Entity3D& e) const;
  // Render the selected camera's view into its preview texture (GL: the
  // overlay shows it; null backend: stats only). False = no camera.
  bool render_camera_preview(int width, int height);
  std::uint64_t camera_preview_texture() const;
  // v18 migration report of the last open ("" = nothing migrated).
  const std::string& camera_migration_backup() const { return camera_backup_; }

  // ---- Attach tools (Editor3DScreen_Attach.cpp, docs/transforms.md) -------
  // Every call works on the selection roots (minus the target), is one undo
  // step and refuses while playing.
  bool attach_selected(std::uint64_t target, scene3d::AttachMode mode);
  bool detach_selected();  // world transforms kept
  // Follow constraint on the selection (offsets keep them where they are).
  bool follow_selected(std::uint64_t target, bool position, bool rotation);
  bool remove_follow_selected();
  // Bones of `target`'s skinned model ("" list = none).
  std::vector<std::string> bone_names(std::uint64_t target);
  bool attach_selected_to_bone(std::uint64_t target, const std::string& bone,
                               scene3d::AttachMode mode);
  // Snap the selection onto `target`'s position (rotation / scale kept).
  bool snap_selected_to(std::uint64_t target);
  // Copy target's position / rotation / scale onto the selection.
  bool copy_transform_to_selected(std::uint64_t from, bool position, bool rotation, bool scale);
  // Drop each root onto the surface below it and turn its up (+Y) to the
  // surface normal.
  bool align_selected_to_surface();
  // Eyedropper: the next viewport (or hierarchy) click picks the target of
  // `action` ("attach_keep", "attach_snap", "follow_pos", "follow_rot",
  // "follow_both", "snap", "copy", "bone_keep", "bone_snap"); Esc cancels.
  void begin_target_pick(const std::string& action);
  bool target_picking() const { return !pick_action_.empty(); }
  bool finish_target_pick(std::uint64_t target);  // runs the action
  void cancel_target_pick() { pick_action_.clear(); }

  // ---- Assets ---------------------------------------------------------------
  struct AssetEntry {
    std::string rel;   // "primitive:cube" or "assets/x.gltf"
    std::string label;
    bool model = false;
    bool image = false;
  };
  const std::vector<AssetEntry>& assets() const { return assets_; }
  void rescan_assets();

  // Animation preview panel (Editor3DScreen_Anim.cpp): one clip of the
  // selected skinned entity, scrubbed or playing, posed in the edit view.
  struct AnimPreview {
    bool open = false;
    bool playing = true;
    float speed = 1.0f;
    float time = 0.0f;
    std::string clip;
  };
  AnimPreview& anim_preview() { return anim_preview_; }
  void anim_preview_tick(float dt);
  // The selected entity posed at the preview time (sets skin_palette);
  // false when nothing to preview.
  bool anim_preview_pose(scene3d::Entity3D* e);
  // The skinned model an entity animates (its animator's model or mesh).
  const render3d::Model* entity_model(const scene3d::Entity3D& e);

  // ---- Prefabs (Editor3DScreen_Prefabs.cpp) ---------------------------------
  // Each is one undo step + autosave (asset files written by Make / Apply /
  // Edit are restored by undo too). See docs/prefabs.md.
  // Selection (with subtrees) -> prefabs/<name>.prefab.json; the selection
  // becomes the first instance. Returns the asset path ("" on failure).
  std::string save_selection_as_prefab(const std::string& name, std::string* error = nullptr);
  // New instance with its first root at pos; selected. Returns that id.
  std::uint64_t place_prefab(const std::string& rel, const render3d::Vec3& pos);
  // key "" = every override of the entity; whole_instance = every entity
  // of its instance. Apply writes the asset and updates all instances.
  bool prefab_apply(std::uint64_t id, const std::string& key = {}, bool whole_instance = false);
  bool prefab_revert(std::uint64_t id, const std::string& key = {}, bool whole_instance = false);
  bool prefab_unpack(std::uint64_t id);
  // Replace an asset (the prefab editor / tools) and update every instance.
  bool prefab_edit_asset(const std::string& rel, const prefab::Asset& asset);
  // Assets changed on disk -> instances update (one undo step). Entities.
  std::size_t prefab_poll();
  // Entity ids of the instance `id` belongs to (empty when not an instance).
  std::vector<std::uint64_t> prefab_instance_members(std::uint64_t id) const;
  PrefabLibrary& prefabs() { return prefabs_; }
  AssetBrowser& asset_browser() { return asset_browser_; }
  VaultPanel& vault_panel() { return vault_panel_; }

  const std::deque<runtime::LogEntry>& log() const { return log_; }
  void note(const std::string& text, runtime::LogLevel level = runtime::LogLevel::Info);

 private:
  struct TerrainSnap {
    terrain::Terrain3D comp;
    terrain::TerrainData data;
  };
  struct Step {
    std::string label;
    scene3d::Scene3D scene;
    std::vector<TerrainSnap> terrains;  // heightfield data changed by this step
  };
  struct Stroke {
    std::uint64_t id = 0;
    terrain::Terrain3D comp;
    terrain::TerrainData before;
    std::uint32_t seed = 1;
    float flatten_target = 0.0f;
    bool has_target = false;
  };
  std::optional<Stroke> stroke_;
  TerrainTool terrain_tool_;
  bool show_terrain_ = false;
  render3d::Vec3 brush_at_{0, 0, 0};
  bool brush_hit_ = false;
  std::optional<scene3d::Scene3D> terrain_edit_before_;
  void draw_terrain_panel();
  bool terrain_viewport(float mx, float my, float vw, float vh, float ox, float oy);
  // Swap snapshots with the live data (undo / redo); returns the current state.
  std::vector<TerrainSnap> swap_terrains(std::vector<TerrainSnap> snaps);
  void push_terrain_undo(const std::string& label, const scene3d::Scene3D& before,
                         std::vector<TerrainSnap> data);
  void push_undo(const std::string& label, const scene3d::Scene3D& before);
  void autosave();
  std::uint64_t add_entity(scene3d::Entity3D e, const std::string& label);
  void ensure_renderer();
  void draw_prefabs_panel();
  void draw_prefab_inspector(const scene3d::Entity3D& e);
  bool refresh_prefab_overrides();
  bool prefab_set_entities(const std::vector<json::Value>& js, const std::string& label,
                           const scene3d::Scene3D& before);
  void prefab_sync_all_on_load();
  void collect_problems();

  // UI (Editor3DScreen_UI.cpp / _Panels.cpp).
  void draw_ui();
  void setup_dock_layout(unsigned int dockspace_id);
  void draw_menu_bar();
  void draw_menus();  // menu bar contents (also harvested for the palette)
  void setup_field_guide();
  void draw_context_menus_for_audit();
  ux::EditorUx ux_{true};
  bool settings_requested_ = false;
  void draw_toolbar();
  void draw_status_bar();
  void draw_viewport();
  void draw_gizmo_overlay(float x0, float y0, float vw, float vh);
  void draw_hierarchy();
  void draw_inspector();
  void draw_assets();
  void draw_environment();
  void draw_anim_preview();
  void draw_telegraph();
  void handle_shortcuts();
  world3d::Input3D poll_play_input() const;
 public:
  bool look_captured() const { return look_captured_; }
 private:
  void draw_vr_mirror();
  void draw_context_menu(std::uint64_t clicked);
  // Cameras + attach UI.
  void migrate_cameras_on_open(const std::string& path);
  void draw_camera_lines();  // frustums (edit view, after view_->begin)
  void draw_camera_overlay(float x0, float y0, float vw, float vh);
  void draw_camera_inspector(const scene3d::Entity3D& e);
  void draw_attach_menu(std::uint64_t clicked);
  void draw_object_menu();
  bool attach_target_list(const char* id, std::uint64_t* out);
  bool ray_surface(const render3d::Ray& ray, const std::vector<std::uint64_t>& skip, float max_t,
                   float* t);
  std::uint64_t look_through_ = 0;
  std::string camera_backup_;
  std::string pick_action_;
  bool copy_pos_ = true, copy_rot_ = true, copy_scale_ = false;
  std::unique_ptr<render3d::Renderer3D> preview_renderer_;
  std::unique_ptr<render3d::SceneView> preview_view_;
  void draw_hierarchy_row(const scene3d::Entity3D& e, int depth);
  std::size_t duplicate_roots(bool offset);
  // Move one entity by the delta prev -> its current transform's children.
  void carry_descendants(std::uint64_t id, const render3d::Transform& prev);
  xr::SimDrive poll_sim_drive();

  ProjectInfo project_;
  bool back_requested_ = false;
  bool quit_requested_ = false;
  scene3d::Scene3D scene_;
  PrefabLibrary prefabs_;
  AssetBrowser asset_browser_;
  bool show_asset_browser_ = true;
  double asset_poll_clock_ = 0.0;
  void open_asset_browser();
  // Pull a Vault item dropped on the viewport, then place it there.
  void drop_vault_item(const std::string& item, const render3d::Vec3& pos);
  // The Vault (asset library shared by every claim).
  VaultPanel vault_panel_;
  bool show_vault_ = true;
  bool show_prefabs_ = true;
  char prefab_name_[64] = "Prefab";
  double prefab_poll_clock_ = 0.0;
  std::vector<Step> undo_;
  std::vector<Step> redo_;
  std::unique_ptr<render3d::Renderer3D> renderer_;
  std::unique_ptr<render3d::SceneView> view_;
  bool gl_ = false;
  Gizmo3D gizmo_;
  // The running modal transform (G / R / S or a gizmo drag).
  struct ModalState {
    ModalTransform m;
    std::optional<scene3d::Scene3D> before;
    std::vector<std::uint64_t> roots;
    std::vector<render3d::Transform> start;  // per root
    std::vector<std::pair<std::uint64_t, render3d::Transform>> all;  // roots + subtrees
    std::size_t active = 0;  // index into roots
    render3d::Vec3 pivot{0, 0, 0};
    float vw = 1, vh = 1, mx = 0, my = 0;
    bool snap = false;
    bool from_gizmo = false;
    // Values shown in the header (degrees / factors / world delta).
    render3d::Vec3 shown{0, 0, 0};
    std::string shown_label;  // undo label ("Move Crate")
  } modal_;
  void modal_apply();
  void draw_transform_header(float x0, float y0, float vw);
  void draw_cursor3d(float x0, float y0, float vw, float vh);
  void handle_modal_keys();
  PivotMode pivot_mode_ = PivotMode::Median;
  XformSpace orientation_ = XformSpace::Global;
  render3d::Vec3 cursor3d_{0, 0, 0};
  bool snap_surface_ = false;
  GizmoHandle gizmo_hover_;
  std::optional<scene3d::Scene3D> inspector_before_;
  std::string inspector_label_;
  world3d::World3D world_;
  bool free_camera_in_play_ = false;
  // Mouse look while riding (Player look = Mouse): the cursor is captured
  // in the viewport; Esc, Pause, Free cam or losing focus free it, a click
  // in the viewport takes it back. ImGui ignores the mouse while captured.
  bool look_captured_ = false;
  bool look_swallow_click_ = false;
  double look_mx_ = 0.0, look_my_ = 0.0;
  void set_look_capture(bool on);
  void update_mouse_look(world3d::Input3D* in);
  std::vector<AssetEntry> assets_;
  std::vector<std::string> scripts_;
  ScriptInfoCache script_infos_;
  AddScriptMenu add_script_menu_;
  bool add_component_open_ = false;
  std::uint64_t script_drop_id_ = 0;
  std::string script_drop_rel_;
  char new_script_name_[64]{};
  int new_script_lang_ = 0;
  int new_script_template_ = 0;
  std::string scripts_selected_;
  ScriptStackHost script_host();
  void draw_scripts_panel();
  void draw_add_component(std::uint64_t id, std::string* attach_rel);
  std::vector<std::string> sounds_;  // project sounds (Audio Source picker)
  bool show_mixer_ = false;
  AnimPreview anim_preview_;
  // Sound while riding (Play or VR preview); null otherwise.
  std::unique_ptr<audio::SoundStage> sound_stage_;
  std::unique_ptr<audio::SoundStage3D> sound3d_;
  void update_sound(double dt);
  void end_sound();
  std::deque<runtime::LogEntry> log_;
  std::deque<std::pair<std::string, double>> toasts_;
  PlayUi play_ui_;
  // ts.profiler: in-game overlay (F3) + console (backtick) over the Play UI.
  profiler::DebugTools debug_tools_;
  ConsoleLineState telegraph_console_;
  bool debug_tools_on_ = false;
  UiPreview ui_preview_;
  bool show_ui_preview_ = false;  // the project's ui/ documents over the Play view
  render3d::FrameStats last_stats_;
  bool dock_initialized_ = false;
  bool viewport_hovered_ = false;
  bool viewport_focused_ = false;
  bool show_colliders_ = true;
  bool show_hitboxes_ = false;  // View > Hitboxes (Source $hbox)
  bool show_physics_ = false;  // Physics Settings window
  void draw_physics_settings();
  float viewport_w_ = 1280.0f, viewport_h_ = 720.0f;
  float viewport_x_ = 0.0f, viewport_y_ = 0.0f;  // screen position of the view
  bool open_pivot_menu_ = false;                 // '.' pressed
  bool swallow_lmb_ = false;                     // the click that confirmed
  char rename_buf_[128] = {0};
  PluginsPanelState plugins_panel_;
  std::unique_ptr<xr::XrSession> vr_session_;
  std::unique_ptr<xr::VrPresenter> vr_presenter_;
  xr::VrFrameResult vr_frame_;
  std::string vr_status_;
  bool vr_mirror_hovered_ = false;
  float vr_last_mx_ = 0.0f, vr_last_my_ = 0.0f;
  bool dup_drag_ = false;          // gizmo drag is an Alt+drag copy
  bool surface_snap_ = false;
  bool rmb_dragged_ = false;       // RMB press orbited/flew (no menu)
  std::uint64_t context_id_ = 0;
  char hier_filter_[96] = {0};
  bool hier_open_context_ = false;
  std::uint64_t hier_drop_id_ = 0, hier_drop_target_ = 0;
  int hier_drop_where_ = 1;
  bool hier_drop_root_ = false;
  std::uint64_t hier_toggle_lock_ = 0, hier_toggle_hide_ = 0;
  // Last member: unhitched first.
  EditorPlugins plugins_;
  std::map<std::string, bool> plugin_panel_open_;  // plugin panels without their own flag
};

// Animator inspector body (Editor3DScreen_Anim.cpp): params, layers with
// masks, states (clip / blend space), transitions, events, root motion.
// `after(label)` follows each drag/text widget; discrete edits set
// *commit. `model` (may be null) lists clip and bone names.
void draw_animator_fields(scene3d::Animator3D& a, const render3d::Model* model,
                          const std::function<void(const char*)>& after, const char** commit);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
