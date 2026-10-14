#pragma once

// Drives one VR frame for a World3D in VR mode through an XrSession:
// pump events, wait for the frame, feed head + hands to the world, step
// it, send haptics back, render both eyes (scene + VR overlay) into the
// session's targets and submit. Used by ts_game, the editor's "Preview in
// headset" and the VR smokes.

#include "render3d/Renderer3D.h"
#include "render3d/SceneView.h"
#include "world3d/World3D.h"
#include "xr/XrSession.h"

namespace ts {
namespace tombstone {
namespace xr {

struct VrFrameResult {
  bool started = false;    // wait_frame gave a frame
  bool rendered = false;   // both eyes drawn
  int ticks = 0;           // world ticks run
  render3d::FrameStats eyes[2];
  render3d::Camera3D eye_cameras[2];
};

class VrPresenter {
 public:
  VrPresenter(render3d::Renderer3D* renderer, render3d::SceneView* view)
      : r_(renderer), view_(view) {}

  // Renderer state to restore after the eyes (the editor renders its own
  // viewport offscreen).
  void set_restore_offscreen(bool on) { restore_offscreen_ = on; }
  // Extra entities drawn in both eyes until replaced (the VR UI panel).
  void set_extra_entities(std::vector<scene3d::Entity3D> extra) { extra_ = std::move(extra); }

  // One frame. real_seconds: wall time since the last frame (world step).
  // False when the session wants to stop (exiting / lost).
  bool frame(XrSession& session, world3d::World3D& world, double real_seconds,
             VrFrameResult* out = nullptr);

  // Just the eyes for given views (no wait / submit): smokes and mirrors.
  void render_eyes(XrSession& session, const world3d::World3D& world, const FrameInfo& f,
                   VrFrameResult* out);

  // Eye camera in the world for a tracking-space view.
  static render3d::Camera3D eye_camera(const world3d::World3D& world, const EyeView& v);

 private:
  render3d::Renderer3D* r_;
  render3d::SceneView* view_;
  bool restore_offscreen_ = false;
  std::vector<scene3d::Entity3D> extra_;
};

}  // namespace xr
}  // namespace tombstone
}  // namespace ts
