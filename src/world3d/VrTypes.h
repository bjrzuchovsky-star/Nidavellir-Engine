#pragma once

// VR data shared by the XR backends (ts_xr: OpenXR and the desktop
// simulator) and the VR play runtime (World3D VR mode). Poses come in the
// tracking space ("stage": floor at y = 0, metres, -Z forward) and World3D
// moves them into the world with the player rig.

#include "render3d/Math3D.h"

#include <cstdint>

namespace ts {
namespace tombstone {
namespace world3d {

enum VrHand : int { kLeftHand = 0, kRightHand = 1 };
inline const char* hand_name(int hand) { return hand == kLeftHand ? "left" : "right"; }

struct VrPose {
  render3d::Vec3 position;
  render3d::Quat orientation;
  bool valid = false;
  // -Z of the pose (where a controller's aim points / the head looks).
  render3d::Vec3 forward() const { return orientation.rotate({0.0f, 0.0f, -1.0f}); }
};

struct VrHandInput {
  VrPose grip;   // where the hand holds things
  VrPose aim;    // laser origin + direction
  float trigger = 0.0f;  // 0..1
  float squeeze = 0.0f;  // 0..1 (grip)
  float stick_x = 0.0f;  // -1..1 thumbstick / trackpad
  float stick_y = 0.0f;  // -1..1 (+ = forward)
  bool primary = false;    // A / X (Index, Touch); trackpad click (Vive)
  bool secondary = false;  // B / Y
  bool menu = false;       // menu / system-adjacent button
};

struct VrInput {
  VrPose head;
  VrHandInput hands[2];
};

struct VrHaptic {
  int hand = kRightHand;
  float amplitude = 0.5f;  // 0..1
  float seconds = 0.05f;
};

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
