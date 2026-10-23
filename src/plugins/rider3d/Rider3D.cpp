// Rider 3D: first / third person movement and look (what World3D's
// move_player / mouse look did before 0.2.0, now behind
// world3d::RiderController3D).

#include "plugins/rider3d/Rider3D.h"

#include "world3d/RiderController3D.h"
#include "world3d/World3D.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace ts {
namespace tombstone {
namespace plugins {

namespace {

using render3d::Quat;
using render3d::Vec3;
using scene3d::Player3D;
using world3d::World3D;

class Rider3D final : public world3d::RiderController3D {
 public:
  bool mouse_look(const Player3D& cfg) const override {
    return cfg.look == Player3D::Look::Mouse;
  }

  void look_mouse(world3d::RiderMotion& m, float dx, float dy) override {
    const Player3D& cfg = m.cfg;
    const float sens = std::clamp(cfg.mouse_sensitivity, Player3D::kMinSensitivity,
                                  Player3D::kMaxSensitivity);
    m.want_yaw -= dx * sens;                            // right turns right
    m.want_pitch += (cfg.invert_y ? dy : -dy) * sens;   // up looks up
    clamp_look(m);
  }

  void steer(world3d::RiderMotion& m, const world3d::Input3D& in, float dt) override {
    const Player3D& cfg = m.cfg;
    const bool first_person = cfg.view == Player3D::View::FirstPerson;
    // Look: keys (Q / Z, Page Up / Down), the right stick, then the mouse
    // (already in want_*) all steer the target; smoothing eases toward it.
    m.want_yaw += in.turn;
    m.want_pitch += in.look;
    const float stick_step = World3D::kStickLookSpeed * static_cast<float>(World3D::kTickSeconds);
    m.want_yaw -= std::clamp(in.stick_x, -1.0f, 1.0f) * stick_step;
    m.want_pitch += std::clamp(cfg.invert_y ? in.stick_y : -in.stick_y, -1.0f, 1.0f) * stick_step;
    clamp_look(m);
    ease_look(m);
    const float yaw = render3d::radians(m.yaw);
    const Vec3 fwd{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    const Vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
    Vec3 wish = fwd * std::clamp(in.move_forward, -1.0f, 1.0f) +
                right * std::clamp(in.move_right, -1.0f, 1.0f);
    if (render3d::length(wish) > 1.0f) wish = render3d::normalize(wish);
    (void)dt;
    // Ground control snaps, air control eases.
    const float accel = m.grounded ? 1.0f : 0.15f;
    m.velocity.x += (wish.x * cfg.speed - m.velocity.x) * accel;
    m.velocity.z += (wish.z * cfg.speed - m.velocity.z) * accel;
    if (first_person) {
      m.rider.transform.rotation = Quat::axis_angle({0, 1, 0}, yaw);  // faces the view
    } else if (render3d::length(wish) > 0.05f) {
      m.rider.transform.rotation = Quat::axis_angle({0, 1, 0}, std::atan2(-wish.x, -wish.z));
    }
    if (in.jump && m.grounded) m.velocity.y = cfg.jump_speed;
  }

 private:
  static void clamp_look(world3d::RiderMotion& m) {
    if (m.cfg.view == Player3D::View::FirstPerson) {
      m.want_pitch = std::clamp(m.want_pitch, -World3D::kFirstPersonPitchLimit,
                                World3D::kFirstPersonPitchLimit);
    } else {
      m.want_pitch = std::clamp(m.want_pitch, World3D::kThirdPersonPitchMin,
                                World3D::kThirdPersonPitchMax);
    }
    // Keep yaw small without a visible jump (both move together).
    if (std::fabs(m.want_yaw) > 720.0f) {
      const float wrap = 360.0f * std::trunc(m.want_yaw / 360.0f);
      m.want_yaw -= wrap;
      m.yaw -= wrap;
    }
  }

  static void ease_look(world3d::RiderMotion& m) {
    const float smooth = std::clamp(m.cfg.smoothing, 0.0f, Player3D::kMaxSmoothing);
    if (smooth <= 0.0f) {
      m.yaw = m.want_yaw;
      m.pitch = m.want_pitch;
      return;
    }
    const float k = 1.0f - smooth;
    m.yaw += (m.want_yaw - m.yaw) * k;
    m.pitch += (m.want_pitch - m.pitch) * k;
    if (std::fabs(m.want_yaw - m.yaw) < 1e-3f) m.yaw = m.want_yaw;
    if (std::fabs(m.want_pitch - m.pitch) < 1e-3f) m.pitch = m.want_pitch;
  }
};

}  // namespace

void offer_rider3d() {
  using Ptr = std::shared_ptr<world3d::RiderController3D>;
  world3d::RiderControllers3D::instance().offer(world3d::kRider3DPluginId,
                                                [] { return Ptr(std::make_shared<Rider3D>()); });
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
