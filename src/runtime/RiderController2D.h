#pragma once

// 2D rider movement interface. The core World owns riders, input slots and
// collision (move_actor sweeps against tiles, solids and bodies); how a
// rider turns input into motion is a Trading Post plugin's job:
//
//   ts.move.topdown     Top-down Rider: 8-way stick / WASD, Face mouse aim
//   ts.move.platformer  Platformer Rider: walk, gravity, jump, ceilings
//
// The plugin offers its controller (core::Providers) and holds the id while
// it is on for the claim; World::move_rider picks one by the player's mode
// (scene.json "player" "mode"). None on: the rider stays put (one warning
// per ride). The rider data (speed, gravity, jump speed, face_mouse) stays
// in the scene format, so the inspector is unchanged.

#include "core/Providers.h"
#include "runtime/Input.h"

#include <memory>

namespace ts {
namespace tombstone {
namespace runtime {

class World;
struct Actor;

inline constexpr const char* kTopDownRiderId = "ts.move.topdown";
inline constexpr const char* kPlatformerRiderId = "ts.move.platformer";

class RiderController2D {
 public:
  virtual ~RiderController2D() = default;
  // One fixed tick (dt seconds) of `a` from `in`: move it with
  // world.move_actor (collides), then leave in_x / in_y, vx / vy (what
  // actually happened), facing, grounded / fall_vy and aiming / aim_deg as
  // the animation, scripts and the network expect.
  virtual void move(World& world, Actor& a, const PlayerInput& in, float dt) = 0;
};

using RiderControllers2D = core::Providers<std::shared_ptr<RiderController2D>>;

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
