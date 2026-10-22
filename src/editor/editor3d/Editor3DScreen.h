#pragma once
#include <map>
#include "debugtools/DebugTools.h"
#include "editor/plugins/ProfilerPanels.h"

// The 3D editor ("dimension": "3d" claims). Docked panels in the Dusk
// theme:
//   Viewport3D  GL 3.3 render of the scene (offscreen target shown as an
//               image): orbit (RMB / Alt+LMB), pan (MMB), zoom (wheel), fly
//               (hold RMB + WASD/QE), F frames the selection, click picks,
//               W / E / R translate / rotate / scale gizmo with snap.
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
#include "editor/team/TeamSession.h"
#include "editor/editor3d/Gizmo3D.h"
#include "editor/plugins/EditorPlugins.h"
#include "editor/plugins/PluginsPanel.h"
#include "editor/screens/IScreen.h"
#include "editor/ui/PlayUi.h"
#include "render3d/Renderer3D.h"
#include "render3d/Scene3D.h"
#include "render3d/SceneView.h"
#include "terrain/Terrain.h"
#include "terrain/TerrainComponent.h"
#include "runtime/Log.h"
#include "world3d/World3D.h"
#include "xr/VrPresenter.h"
#include "xr/XrSession.h"

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
  // Gizmo drag on the primary selection: begin (false on a miss), move,
  // end (one undo step). Same code the mouse drives.
  bool gizmo_begin(float px, float py, float vw, float vh);
  bool gizmo_begin_axis(int axis, float px, float py, float vw, float vh);
  void gizmo_move(float px, float py, float vw, float vh);
  void gizmo_end();
  void gizmo_cancel();

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
  // Translate drags stick to surfaces under the mouse while on (Ctrl).
  void set_surface_snap(bool on) { surface_snap_ = on; }

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
  std::optional<scene3d::Scene3D> gizmo_before_;
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
  bool show_physics_ = false;  // Physics Settings window
  void draw_physics_settings();
  float viewport_w_ = 1280.0f, viewport_h_ = 720.0f;
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
 public:
  // Live Team Claims (src/editor/team, docs/team-claims.md).
  TeamSession& team();
  bool has_team() const { return team_ != nullptr; }
  bool team_panel_open() const { return show_team_; }
  void set_team_panel_open(bool on) { show_team_ = on; }
  // Called by on_update; public so --smoke-team can drive headless editors.
  void tick_team(float dt);

 private:
  friend class Team3DBinding;
  void draw_team_panel();
  bool show_team_ = false;
  // Last member: destroyed first (its binding points back at this screen).
  std::unique_ptr<TeamSession> team_;
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
