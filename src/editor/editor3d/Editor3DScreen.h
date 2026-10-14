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
// Every edit is one undo step (Ctrl+Z / Ctrl+Y) and autosaves scene.json
// (v7). Play (F5) rides the scene in a World3D copy; Stop restores it.

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
  // Last member: unhitched first.
  EditorPlugins plugins_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
