#pragma once

// Perspective camera for 3D / VR projects. In VR the XR session overrides
// view + projection per eye; the camera then is the rig origin.

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
  // VR eyes: explicit view + projection (asymmetric per-eye frustum). When
  // set, view() / projection() return them; position / target still
  // describe the eye (lighting, shadow focus).
  bool explicit_matrices = false;
  Mat4 view_matrix;
  Mat4 projection_matrix;

  Mat4 view() const {
    return explicit_matrices ? view_matrix : Mat4::look_at(position, target, up);
  }
  Mat4 projection(float aspect) const {
    return explicit_matrices ? projection_matrix
                             : Mat4::perspective(radians(fov_y_degrees), aspect, z_near, z_far);
  }
  Vec3 forward() const { return normalize(target - position); }

  // World ray through a pixel (0,0 = top-left) of a width x height view.
  Ray screen_ray(float px, float py, float width, float height) const {
    const float aspect = height > 0.0f ? width / height : 1.0f;
    const Mat4 inv = (projection(aspect) * view()).inverse();
    const float nx = (2.0f * px / (width > 0 ? width : 1.0f)) - 1.0f;
    const float ny = 1.0f - (2.0f * py / (height > 0 ? height : 1.0f));
    const Vec3 a = inv.transform_point({nx, ny, -1.0f});
    const Vec3 b = inv.transform_point({nx, ny, 1.0f});
    return {a, normalize(b - a)};
  }

  // World point to pixel coordinates; false when behind the camera.
  bool project(const Vec3& p, float width, float height, float* sx,
               float* sy) const {
    const float aspect = height > 0.0f ? width / height : 1.0f;
    const Mat4 vp = projection(aspect) * view();
    const float x = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
    const float y = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
    const float w = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
    if (w <= 1e-5f) return false;
    *sx = (x / w * 0.5f + 0.5f) * width;
    *sy = (1.0f - (y / w * 0.5f + 0.5f)) * height;
    return true;
  }
};

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
