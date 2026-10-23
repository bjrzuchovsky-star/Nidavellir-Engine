// Top-down and platformer riders (what World::move_rider did before 0.2.0,
// now behind runtime::RiderController2D).

#include "plugins/movement2d/Riders2D.h"

#include "runtime/RiderController2D.h"
#include "runtime/World.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace ts {
namespace tombstone {
namespace plugins {

namespace {

using runtime::Actor;
using runtime::PlayerInput;
using runtime::World;

// Top-down "face mouse": turn the sprite toward the aim point.
void face_aim(Actor& a, const PlayerInput& in) {
  if (!a.data.player || !a.data.player->face_mouse) {
    a.aiming = false;
    return;
  }
  a.aiming = true;
  if (!in.aim) return;  // no cursor this tick: keep the last heading
  const float dx = in.aim_x - (a.data.x + a.data.w * 0.5f);
  const float dy = in.aim_y - (a.data.y + a.data.h * 0.5f);
  if (dx * dx + dy * dy < 1.0f) return;  // on top of the rider: hold still
  a.aim_deg = runtime::top_down_aim_degrees(dx, dy);
}

class TopDownRider final : public runtime::RiderController2D {
 public:
  void move(World& world, Actor& a, const PlayerInput& in, float dt) override {
    float mx = std::clamp(in.move_x, -1.0f, 1.0f);
    float my = std::clamp(in.move_y, -1.0f, 1.0f);
    // Diagonals are no faster than straight lines.
    const float len = std::sqrt(mx * mx + my * my);
    if (len > 1.0f) {
      mx /= len;
      my /= len;
    }
    a.in_x = mx;
    a.in_y = my;
    const float x0 = a.data.x;
    const float y0 = a.data.y;
    world.move_actor(a, mx * a.data.player->speed * dt, my * a.data.player->speed * dt);
    face_aim(a, in);
    // Report what actually happened (0 when stopped flush on a wall).
    a.vx = (a.data.x - x0) / dt;
    a.vy = (a.data.y - y0) / dt;
    if (mx > 0.0f) {
      a.facing = 1;
    } else if (mx < 0.0f) {
      a.facing = -1;
    }
  }
};

class PlatformerRider final : public runtime::RiderController2D {
 public:
  void move(World& world, Actor& a, const PlayerInput& in, float dt) override {
    // Side-on: the stick walks, gravity pulls, up / action jumps off the
    // ground. Collision is the same axis-at-a-time sweep as top-down.
    const PlayerControllerData& pc = *a.data.player;
    const float x0 = a.data.x;
    const float y0 = a.data.y;
    const float walk = std::clamp(in.move_x, -1.0f, 1.0f);
    a.in_x = walk;
    a.in_y = 0.0f;
    a.aiming = false;
    const bool jump = in.move_y < -0.5f || in.held(runtime::kButtonAction);
    if (jump && a.grounded) {
      a.fall_vy = -pc.jump_speed;
      a.grounded = false;
    }
    a.fall_vy = std::min(a.fall_vy + pc.gravity * dt, 2400.0f);
    const float want_dy = a.fall_vy * dt;
    world.move_actor(a, walk * pc.speed * dt, 0.0f);
    const float before = a.data.y;
    world.move_actor(a, 0.0f, want_dy);
    const float got_dy = a.data.y - before;
    a.grounded = false;
    if (want_dy > 0.0f && got_dy < want_dy - 0.01f) {
      a.grounded = true;  // landed
      a.fall_vy = 0.0f;
    } else if (want_dy < 0.0f && got_dy > want_dy + 0.01f) {
      a.fall_vy = 0.0f;  // bumped a ceiling
    }
    a.vx = (a.data.x - x0) / dt;
    a.vy = (a.data.y - y0) / dt;
    if (walk > 0.0f) {
      a.facing = 1;
    } else if (walk < 0.0f) {
      a.facing = -1;
    }
  }
};

}  // namespace

void offer_2d_riders() {
  using Ptr = std::shared_ptr<runtime::RiderController2D>;
  runtime::RiderControllers2D& r = runtime::RiderControllers2D::instance();
  r.offer(runtime::kTopDownRiderId, [] { return Ptr(std::make_shared<TopDownRider>()); });
  r.offer(runtime::kPlatformerRiderId, [] { return Ptr(std::make_shared<PlatformerRider>()); });
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
