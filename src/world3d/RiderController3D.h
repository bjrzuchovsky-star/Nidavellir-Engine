#pragma once

// 3D rider movement interface. World3D keeps the rider, gravity, the sweep
// (the physics world's character when a physics plugin is on, else the core
// capsule-vs-box collision), the fall-off-the-map reset, the follow camera,
// scroll zoom and the whole VR rig. Turning input into motion and look is a
// Trading Post plugin's job:
//
//   ts.move.rider3d  Rider 3D: first / third person, WASD / stick, jump,
//                    look keys + right stick, mouse look (Sensitivity,
//                    Invert Y, Smoothing, cursor capture)
//
// The plugin offers its controller (core::Providers) and holds the id while
// it is on for the claim; World3D picks it up at start(). None on: the rider
// still falls and lands but does not walk or look (one warning per ride)
// and the host never captures the cursor. Player3D settings stay in the
// scene format, so the inspector is unchanged.

#include "core/Providers.h"
#include "render3d/Math3D.h"

#include <memory>

namespace ts {
namespace tombstone {
namespace scene3d {
struct Entity3D;
struct Player3D;
}  // namespace scene3d

namespace world3d {

struct Input3D;

inline constexpr const char* kRider3DPluginId = "ts.move.rider3d";

// The rider as the controller may change it this tick.
struct RiderMotion {
  scene3d::Entity3D& rider;          // transform.rotation: facing
  const scene3d::Player3D& cfg;      // speed, jump, view, look settings
  render3d::Vec3& velocity;          // m/s; World3D adds gravity after
  bool grounded;                     // standing on something (last sweep)
  float& yaw;                        // view yaw now (degrees, + = left)
  float& pitch;                      // view pitch now (degrees, + = up)
  float& want_yaw;                   // look targets smoothing eases toward
  float& want_pitch;
};

class RiderController3D {
 public:
  virtual ~RiderController3D() = default;
  // Hosts capture / read the mouse for this rider (Player look = Mouse).
  virtual bool mouse_look(const scene3d::Player3D& cfg) const = 0;
  // Raw mouse counts since the last call (+x right, +y down): turn the
  // look targets (sensitivity, invert Y, pitch limits).
  virtual void look_mouse(RiderMotion& m, float dx, float dy) = 0;
  // One fixed tick: look from keys / stick, smoothing, walk / air control,
  // facing and jump (set velocity.y). Called before gravity and the sweep.
  virtual void steer(RiderMotion& m, const Input3D& in, float dt) = 0;
};

using RiderControllers3D = core::Providers<std::shared_ptr<RiderController3D>>;

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
