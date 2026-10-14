#include "xr/VrPresenter.h"

#include "render3d/GlApi.h"
#include "world3d/VrOverlay.h"

namespace ts {
namespace tombstone {
namespace xr {

using render3d::Camera3D;
using render3d::Mat4;

Camera3D VrPresenter::eye_camera(const world3d::World3D& world, const EyeView& v) {
  world3d::VrPose p;
  p.position = v.position;
  p.orientation = v.orientation;
  p.valid = true;
  const world3d::VrPose w = world.to_world(p);
  Camera3D c;
  c.position = w.position;
  c.target = w.position + w.forward();
  c.up = w.orientation.rotate({0, 1, 0});
  c.z_near = 0.05f;
  c.z_far = 500.0f;
  c.explicit_matrices = true;
  c.view_matrix = Mat4::trs(w.position, w.orientation, {1, 1, 1}).inverse();
  c.projection_matrix =
      Mat4::perspective_fov(v.fov_left, v.fov_right, v.fov_up, v.fov_down, c.z_near, c.z_far);
  return c;
}

void VrPresenter::render_eyes(XrSession& session, const world3d::World3D& world,
                              const FrameInfo& f, VrFrameResult* out) {
  if (!r_ || !view_ || f.views.size() < 2) return;
  std::vector<scene3d::Entity3D> ents = world.entities();
  // Hide the rig's own mesh (if any) from the inside.
  world3d::vr_overlay_entities(world, true, &ents);
  ents.insert(ents.end(), extra_.begin(), extra_.end());
  const int w = session.eye_width(), h = session.eye_height();
  GLint prev_fbo = 0, prev_vp[4] = {0, 0, 0, 0};
  const bool gl = std::string(r_->backend_name()) == "gl";
  if (gl) {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_VIEWPORT, prev_vp);
  }
  r_->set_offscreen(false);
  bool all = true;
  for (int e = 0; e < 2; ++e) {
    const Camera3D cam = eye_camera(world, f.views[e]);
    if (out) out->eye_cameras[e] = cam;
    if (gl && !session.bind_eye(e)) {
      all = false;
      continue;
    }
    view_->begin(world.authored().environment, ents, cam, w, h);
    world3d::vr_overlay_lines(world, r_);
    const render3d::FrameStats st = view_->end();
    if (out) out->eyes[e] = st;
  }
  if (restore_offscreen_) r_->set_offscreen(true);
  if (gl) {
    render3d::gl33().BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
  }
  if (out) out->rendered = all;
}

bool VrPresenter::frame(XrSession& session, world3d::World3D& world, double real_seconds,
                        VrFrameResult* out) {
  if (!session.poll_events()) return false;
  if (!session.running()) return true;  // waiting for the runtime (READY)
  FrameInfo f;
  if (!session.wait_frame(&f)) return true;
  if (out) out->started = true;
  world.set_vr_input(session.input());
  const int ticks = world.update(real_seconds, world3d::Input3D{});
  if (out) out->ticks = ticks;
  for (const world3d::VrHaptic& h : world.take_haptics()) session.haptic(h);
  if (f.should_render) render_eyes(session, world, f, out);
  session.end_frame();
  return true;
}

}  // namespace xr
}  // namespace tombstone
}  // namespace ts
