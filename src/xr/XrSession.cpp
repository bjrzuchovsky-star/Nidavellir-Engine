#include "xr/XrSession.h"

#include "render3d/GlApi.h"
#include "xr/EyeTarget.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace xr {

using render3d::Quat;
using render3d::Vec3;

const char* to_string(SessionState s) {
  switch (s) {
    case SessionState::Idle: return "idle";
    case SessionState::Ready: return "ready";
    case SessionState::Synchronized: return "synchronized";
    case SessionState::Visible: return "visible";
    case SessionState::Focused: return "focused";
    case SessionState::Stopping: return "stopping";
    case SessionState::LossPending: return "loss pending";
    case SessionState::Exiting: return "exiting";
    case SessionState::Unknown: break;
  }
  return "unknown";
}

bool XrSession::read_eye(int eye, std::vector<unsigned char>* rgba, int* w, int* h) const {
  const std::uint32_t fb = eye_framebuffer(eye);
  if (!fb || !rgba) return false;
  GLint prev = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
  render3d::gl33().BindFramebuffer(GL_FRAMEBUFFER, fb);
  const int ew = eye_width(), eh = eye_height();
  rgba->assign(static_cast<std::size_t>(ew) * eh * 4, 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, ew, eh, GL_RGBA, GL_UNSIGNED_BYTE, rgba->data());
  render3d::gl33().BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev));
  if (w) *w = ew;
  if (h) *h = eh;
  return true;
}

// ---- Simulator ------------------------------------------------------------------

SimXrSession::SimXrSession(const XrConfig& config) : cfg_(config) {
  w_ = std::max(16, config.sim_eye_width);
  h_ = std::max(16, config.sim_eye_height);
  compute_input();
}

SimXrSession::~SimXrSession() { end(); }

bool SimXrSession::begin(std::string* error) {
  end();
  gl_ = false;
  std::string why;
  if (glfwGetCurrentContext() && render3d::load_gl33(&why)) {
    for (int e = 0; e < 2; ++e) {
      EyeTarget t;
      if (!t.create(w_, h_)) {
        if (error) *error = "simulator: eye framebuffer incomplete";
        end();
        return false;
      }
      fbo_[e] = t.fbo;
      color_[e] = t.color;
      depth_[e] = t.depth;
    }
    gl_ = true;
  }
  running_ = true;
  state_ = SessionState::Focused;
  frames_ = 0;
  return true;
}

void SimXrSession::end() {
  if (gl_ && glfwGetCurrentContext()) {
    for (int e = 0; e < 2; ++e) {
      EyeTarget t;
      t.fbo = fbo_[e];
      t.color = color_[e];
      t.depth = depth_[e];
      t.destroy();
    }
  }
  for (int e = 0; e < 2; ++e) fbo_[e] = color_[e] = depth_[e] = 0;
  gl_ = false;
  running_ = false;
  if (state_ != SessionState::Exiting) state_ = SessionState::Idle;
}

void SimXrSession::compute_input() {
  const Quat q = render3d::normalize(
      Quat::axis_angle({0, 1, 0}, render3d::radians(c_.yaw_deg)) *
      Quat::axis_angle({1, 0, 0}, render3d::radians(c_.pitch_deg)));
  if (c_.hands_follow_head) {
    input_.head.position = c_.head;
    input_.head.orientation = q;
    input_.head.valid = true;
    for (int h = 0; h < 2; ++h) {
      world3d::VrHandInput hand = c_.buttons[h];
      const float side = h == world3d::kLeftHand ? -0.18f : 0.18f;
      hand.grip.position = c_.head + q.rotate({side, -0.22f, -c_.reach});
      hand.grip.orientation = q;
      hand.grip.valid = true;
      hand.aim.position = hand.grip.position + q.rotate({0.0f, 0.02f, -0.06f});
      hand.aim.orientation = q;
      hand.aim.valid = true;
      input_.hands[h] = hand;
    }
  } else {
    input_ = c_.manual;
    if (!input_.head.valid) {
      input_.head.position = c_.head;
      input_.head.orientation = q;
      input_.head.valid = true;
    }
    for (int h = 0; h < 2; ++h) {
      const world3d::VrHandInput& b = c_.buttons[h];
      input_.hands[h].trigger = b.trigger;
      input_.hands[h].squeeze = b.squeeze;
      input_.hands[h].stick_x = b.stick_x;
      input_.hands[h].stick_y = b.stick_y;
      input_.hands[h].primary = b.primary;
      input_.hands[h].secondary = b.secondary;
      input_.hands[h].menu = b.menu;
    }
  }
}

void SimXrSession::drive(const SimDrive& d, double dt) {
  const float fdt = static_cast<float>(std::min(dt, 0.1));
  c_.yaw_deg -= d.look_dx * 0.15f;
  c_.pitch_deg = std::clamp(c_.pitch_deg - d.look_dy * 0.15f, -85.0f, 85.0f);
  if (c_.yaw_deg > 360.0f || c_.yaw_deg < -360.0f) c_.yaw_deg = std::fmod(c_.yaw_deg, 360.0f);
  const float want_y = d.crouch ? c_.standing_height - 0.6f : c_.standing_height;
  c_.head.y += (want_y - c_.head.y) * std::min(1.0f, fdt * 10.0f);
  if (d.reach_out) c_.reach = std::min(0.9f, c_.reach + fdt * 0.5f);
  if (d.reach_in) c_.reach = std::max(0.12f, c_.reach - fdt * 0.5f);
  world3d::VrHandInput& L = c_.buttons[world3d::kLeftHand];
  world3d::VrHandInput& R = c_.buttons[world3d::kRightHand];
  L.stick_x = (d.right ? 1.0f : 0.0f) - (d.left ? 1.0f : 0.0f);
  L.stick_y = (d.forward ? 1.0f : 0.0f) - (d.back ? 1.0f : 0.0f);
  R.stick_x = (d.turn_right ? 1.0f : 0.0f) - (d.turn_left ? 1.0f : 0.0f);
  R.stick_y = d.teleport ? 1.0f : 0.0f;
  world3d::VrHandInput& act = d.left_hand ? L : R;
  world3d::VrHandInput& idle = d.left_hand ? R : L;
  act.trigger = d.trigger ? 1.0f : 0.0f;
  act.squeeze = d.grip ? 1.0f : 0.0f;
  idle.trigger = 0.0f;
  idle.squeeze = 0.0f;
  L.menu = d.menu;
}

bool SimXrSession::wait_frame(FrameInfo* out) {
  if (!running_) return false;
  compute_input();
  time_ns_ += 11111111;
  if (out) {
    out->predicted_display_time_ns = time_ns_;
    out->should_render = true;
    out->views.assign(2, EyeView{});
    const Quat q = input_.head.orientation;
    const Vec3 right = q.rotate({1, 0, 0});
    const float tan_h = std::tan(render3d::radians(50.0f));
    const float tan_v = tan_h * static_cast<float>(h_) / static_cast<float>(w_);
    for (int e = 0; e < 2; ++e) {
      EyeView& v = out->views[e];
      v.position = input_.head.position + right * ((e == 0 ? -0.5f : 0.5f) * cfg_.sim_ipd);
      v.orientation = q;
      v.fov_left = -std::atan(tan_h);
      v.fov_right = std::atan(tan_h);
      v.fov_up = std::atan(tan_v);
      v.fov_down = -std::atan(tan_v);
    }
  }
  return true;
}

bool SimXrSession::bind_eye(int eye) {
  if (!gl_ || eye < 0 || eye > 1) return false;
  render3d::gl33().BindFramebuffer(GL_FRAMEBUFFER, fbo_[eye]);
  glViewport(0, 0, w_, h_);
  return true;
}

std::uint32_t SimXrSession::eye_texture(int eye) const { return color_[eye & 1]; }
std::uint32_t SimXrSession::eye_framebuffer(int eye) const { return fbo_[eye & 1]; }

void SimXrSession::haptic(const world3d::VrHaptic& h) {
  last_haptic_[h.hand & 1] = h;
  ++haptics_;
}

std::unique_ptr<SimXrSession> create_sim_session(const XrConfig& config) {
  return std::make_unique<SimXrSession>(config);
}

std::unique_ptr<XrSession> create_xr_session(const XrConfig& config, std::string* why) {
  std::unique_ptr<XrSession> s = create_openxr_session(config);
  if (s && s->available()) return s;
  if (why) *why = s ? s->unavailable_reason() : std::string("OpenXR not built");
  return create_sim_session(config);
}

}  // namespace xr
}  // namespace tombstone
}  // namespace ts
