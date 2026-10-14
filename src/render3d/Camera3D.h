#pragma once

// Perspective camera for 3D / VR projects (sketch). In VR the XR session
// overrides view + projection per eye; the camera then is the rig origin.

#include "render3d/Math3D.h"

namespace ts {
namespace tombstone {
namespace render3d {

struct Camera3D {
  Vec3 position{0.0f, 2.0f, 6.0f};
  Vec3 target{0.0f, 0.0f, 0.0f};
  Vec3 up{0.0f, 1.0f, 0.0f};
  float fov_y_degrees = 60.0f;
  float z_near = 0.1f;
  float z_far = 1000.0f;

  Mat4 view() const { return Mat4::look_at(position, target, up); }
  Mat4 projection(float aspect) const {
    return Mat4::perspective(fov_y_degrees * 3.14159265f / 180.0f, aspect,
                             z_near, z_far);
  }
};

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
