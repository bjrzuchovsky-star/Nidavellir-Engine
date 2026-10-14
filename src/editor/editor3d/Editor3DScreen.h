#pragma once

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
#include "editor/ProjectInfo.h"
#include "editor/editor3d/Gizmo3D.h"
#include "editor/plugins/EditorPlugins.h"
#include "editor/plugins/PluginsPanel.h"
#include "editor/screens/IScreen.h"
#include "render3d/Renderer3D.h"
#include "render3d/Scene3D.h"
#include "render3d/SceneView.h"
#include "runtime/Log.h"
#include "world3d/World3D.h"
#include "xr/VrPresenter.h"
#include "xr/XrSession.h"

#include <cstdint>
#include <deque>
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

  // ---- Scene + selection --------------------------------------------------
  const scene3d::Scene3D& scene() const { return scene_; }
  const scene3d::Entity3D* find(std::uint64_t id) const { return scene_.find(id); }
  const std::vector<std::uint64_t>& selection() const { return scene_.selection; }
  std::uint64_t primary() const;
  void select(std::uint64_t id, bool additive = false);
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
  bool vr_project() const { return project_.kind == ProjectKind::VR; }
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

  const std::deque<runtime::LogEntry>& log() const { return log_; }
  void note(const std::string& text, runtime::LogLevel level = runtime::LogLevel::Info);

 private:
  struct Step {
    std::string label;
    scene3d::Scene3D scene;
  };
  void push_undo(const std::string& label, const scene3d::Scene3D& before);
  void autosave();
  std::uint64_t add_entity(scene3d::Entity3D e, const std::string& label);
  void ensure_renderer();
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
  void draw_telegraph();
  void handle_shortcuts();
  world3d::Input3D poll_play_input() const;
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
  std::vector<AssetEntry> assets_;
  std::vector<std::string> scripts_;
  std::vector<std::string> sounds_;  // project sounds (Audio Source picker)
  bool show_mixer_ = false;
  // Sound while riding (Play or VR preview); null otherwise.
  std::unique_ptr<audio::SoundStage> sound_stage_;
  std::unique_ptr<audio::SoundStage3D> sound3d_;
  void update_sound(double dt);
  void end_sound();
  std::deque<runtime::LogEntry> log_;
  std::deque<std::pair<std::string, double>> toasts_;
  render3d::FrameStats last_stats_;
  bool dock_initialized_ = false;
  bool viewport_hovered_ = false;
  bool viewport_focused_ = false;
  bool show_colliders_ = true;
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
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
