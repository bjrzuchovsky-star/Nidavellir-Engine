#pragma once

// The survey camp: what a 3D or VR project opens into until those
// renderers ride in. A themed placeholder viewport ("this frontier ain't
// surveyed yet") drawn over a ts_render3d null-backend frame (so the
// interface is exercised every frame), the ts_xr session status for VR,
// the project's Plugins panel, and a small Telegraph.

#include "editor/ProjectInfo.h"
#include "editor/plugins/EditorPlugins.h"
#include "editor/plugins/PluginsPanel.h"
#include "editor/screens/IScreen.h"
#include "render3d/Renderer3D.h"
#include "runtime/Log.h"
#include "xr/XrSession.h"

#include <deque>
#include <memory>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

class FrontierScreen final : public IScreen {
 public:
  explicit FrontierScreen(ProjectInfo project);
  ~FrontierScreen() override;

  AppState state() const override { return AppState::EditorFrontier; }
  void on_enter() override;
  void on_exit() override;
  AppState on_update(float delta_seconds) override;

  void request_back_to_projects() { back_requested_ = true; }
  const ProjectInfo& project() const { return project_; }
  EditorPlugins& plugins() { return plugins_; }

  // One placeholder frame through the 3D interface (headless-safe).
  void render_survey_frame(int width, int height);
  const char* renderer_backend() const;
  const render3d::FrameStats& last_frame() const { return last_frame_; }
  bool xr_available() const { return xr_ && xr_->available(); }
  std::string xr_status() const;
  const std::deque<runtime::LogEntry>& log() const { return log_; }

 private:
  void draw_ui();
  void draw_placeholder_viewport();

  ProjectInfo project_;
  bool back_requested_ = false;
  std::unique_ptr<render3d::Renderer3D> renderer_;
  render3d::MeshHandle cube_;
  render3d::MaterialHandle copper_;
  render3d::FrameStats last_frame_;
  float spin_ = 0.0f;
  std::unique_ptr<xr::XrSession> xr_;
  std::deque<runtime::LogEntry> log_;
  PluginsPanelState plugins_panel_;
  // Last member: unhitched first.
  EditorPlugins plugins_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
