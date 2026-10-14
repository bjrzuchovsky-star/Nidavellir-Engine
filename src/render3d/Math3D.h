#pragma once

// Minimal 3D math for the ts_render3d sketch: just enough for cameras and
// transforms. Column-major 4x4 (OpenGL / Vulkan convention), right-handed,
// -Z forward. A real math library can replace it later without touching
// the interfaces that take these by value.

#include <cmath>

namespace ts {
namespace tombstone {
namespace render3d {

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
  const float l = length(v);
  return l > 0.0f ? v * (1.0f / l) : Vec3{};
}

struct Quat {
  float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

struct Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // column-major

  static Mat4 identity() { return {}; }
  float& at(int row, int col) { return m[col * 4 + row]; }
  float at(int row, int col) const { return m[col * 4 + row]; }

  Mat4 operator*(const Mat4& o) const {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
      for (int rr = 0; rr < 4; ++rr) {
        float s = 0.0f;
        for (int k = 0; k < 4; ++k) s += at(rr, k) * o.at(k, c);
        r.at(rr, c) = s;
      }
    }
    return r;
  }

  static Mat4 perspective(float fov_y_radians, float aspect, float z_near,
                          float z_far) {
    Mat4 r;
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    r.m[0] = f / (aspect > 0.0f ? aspect : 1.0f);
    r.m[5] = f;
    r.m[10] = (z_far + z_near) / (z_near - z_far);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * z_far * z_near) / (z_near - z_far);
    r.m[15] = 0.0f;
    return r;
  }

  static Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 f = normalize(target - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);
    Mat4 r;
    r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z;
    r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z;
    r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z;
    r.at(0, 3) = -dot(s, eye);
    r.at(1, 3) = -dot(u, eye);
    r.at(2, 3) = dot(f, eye);
    return r;
  }
};

struct Transform {
  Vec3 position;
  Quat rotation;
  Vec3 scale{1.0f, 1.0f, 1.0f};
};

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
