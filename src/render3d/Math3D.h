#pragma once

// 3D math for ts_render3d and the 3D runtime: vectors, quaternions and
// column-major 4x4 matrices (OpenGL convention), right-handed, +Y up,
// -Z forward. Header-only, no dependencies.

#include <cmath>

namespace ts {
namespace tombstone {
namespace render3d {

inline constexpr float kPi = 3.14159265358979f;
inline float radians(float deg) { return deg * (kPi / 180.0f); }
inline float degrees(float rad) { return rad * (180.0f / kPi); }

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator-() const { return {-x, -y, -z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
  float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
  float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
  bool operator==(const Vec3& o) const = default;
};

inline Vec3 mul(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
  const float l = length(v);
  return l > 0.0f ? v * (1.0f / l) : Vec3{};
}
inline Vec3 vmin(const Vec3& a, const Vec3& b) {
  return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
inline Vec3 vmax(const Vec3& a, const Vec3& b) {
  return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

struct Vec4 {
  float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

struct Quat {
  float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
  bool operator==(const Quat& o) const = default;

  static Quat axis_angle(const Vec3& axis, float rad) {
    const Vec3 a = normalize(axis);
    const float s = std::sin(rad * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(rad * 0.5f)};
  }
  // Intrinsic Y (yaw), then X (pitch), then Z (roll); degrees.
  static Quat from_euler_deg(const Vec3& e);
  Vec3 to_euler_deg() const;

  Quat operator*(const Quat& b) const {
    return {w * b.x + x * b.w + y * b.z - z * b.y,
            w * b.y - x * b.z + y * b.w + z * b.x,
            w * b.z + x * b.y - y * b.x + z * b.w,
            w * b.w - x * b.x - y * b.y - z * b.z};
  }
  Quat conjugate() const { return {-x, -y, -z, w}; }
  Vec3 rotate(const Vec3& v) const {
    const Vec3 u{x, y, z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * w + cross(u, t);
  }
};

inline Quat normalize(const Quat& q) {
  const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (l <= 0.0f) return {};
  return {q.x / l, q.y / l, q.z / l, q.w / l};
}

inline Quat Quat::from_euler_deg(const Vec3& e) {
  const Quat qy = axis_angle({0, 1, 0}, radians(e.y));
  const Quat qx = axis_angle({1, 0, 0}, radians(e.x));
  const Quat qz = axis_angle({0, 0, 1}, radians(e.z));
  return normalize(qy * qx * qz);
}

inline Vec3 Quat::to_euler_deg() const {
  // Inverse of from_euler_deg (YXZ order).
  const Quat q = normalize(*this);
  const float sinp = 2.0f * (q.w * q.x - q.y * q.z);
  Vec3 out;
  if (std::fabs(sinp) >= 0.9999f) {
    out.x = degrees(std::copysign(kPi * 0.5f, sinp));
    out.y = degrees(std::atan2(-2.0f * (q.x * q.z - q.w * q.y),
                               1.0f - 2.0f * (q.y * q.y + q.z * q.z)));
    out.z = 0.0f;
    return out;
  }
  out.x = degrees(std::asin(sinp));
  out.y = degrees(std::atan2(2.0f * (q.x * q.z + q.w * q.y),
                             1.0f - 2.0f * (q.x * q.x + q.y * q.y)));
  out.z = degrees(std::atan2(2.0f * (q.x * q.y + q.w * q.z),
                             1.0f - 2.0f * (q.x * q.x + q.z * q.z)));
  return out;
}

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

  Vec3 transform_point(const Vec3& p) const {
    const float x = at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3);
    const float y = at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3);
    const float z = at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3);
    const float w = at(3, 0) * p.x + at(3, 1) * p.y + at(3, 2) * p.z + at(3, 3);
    return (w != 0.0f && w != 1.0f) ? Vec3{x / w, y / w, z / w} : Vec3{x, y, z};
  }
  Vec3 transform_dir(const Vec3& d) const {
    return {at(0, 0) * d.x + at(0, 1) * d.y + at(0, 2) * d.z,
            at(1, 0) * d.x + at(1, 1) * d.y + at(1, 2) * d.z,
            at(2, 0) * d.x + at(2, 1) * d.y + at(2, 2) * d.z};
  }

  static Mat4 translate(const Vec3& t) {
    Mat4 r;
    r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z;
    return r;
  }
  static Mat4 scale(const Vec3& s) {
    Mat4 r;
    r.at(0, 0) = s.x; r.at(1, 1) = s.y; r.at(2, 2) = s.z;
    return r;
  }
  static Mat4 rotate(const Quat& q0) {
    const Quat q = normalize(q0);
    Mat4 r;
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    r.at(0, 0) = 1 - 2 * (yy + zz); r.at(0, 1) = 2 * (xy - wz);     r.at(0, 2) = 2 * (xz + wy);
    r.at(1, 0) = 2 * (xy + wz);     r.at(1, 1) = 1 - 2 * (xx + zz); r.at(1, 2) = 2 * (yz - wx);
    r.at(2, 0) = 2 * (xz - wy);     r.at(2, 1) = 2 * (yz + wx);     r.at(2, 2) = 1 - 2 * (xx + yy);
    return r;
  }
  static Mat4 trs(const Vec3& t, const Quat& q, const Vec3& s) {
    return translate(t) * rotate(q) * scale(s);
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

  // Asymmetric frustum from OpenXR-style half-angles (radians; left and
  // down are negative).
  static Mat4 perspective_fov(float left, float right, float up, float down, float z_near,
                              float z_far) {
    const float tl = std::tan(left), tr = std::tan(right);
    const float tu = std::tan(up), td = std::tan(down);
    const float w = tr - tl, h = tu - td;
    Mat4 r;
    r.at(0, 0) = 2.0f / w;
    r.at(0, 2) = (tr + tl) / w;
    r.at(1, 1) = 2.0f / h;
    r.at(1, 2) = (tu + td) / h;
    r.at(2, 2) = -(z_far + z_near) / (z_far - z_near);
    r.at(2, 3) = -(2.0f * z_far * z_near) / (z_far - z_near);
    r.at(3, 2) = -1.0f;
    r.at(3, 3) = 0.0f;
    return r;
  }

  static Mat4 ortho(float l, float r, float b, float t, float n, float f) {
    Mat4 o;
    o.at(0, 0) = 2.0f / (r - l);
    o.at(1, 1) = 2.0f / (t - b);
    o.at(2, 2) = -2.0f / (f - n);
    o.at(0, 3) = -(r + l) / (r - l);
    o.at(1, 3) = -(t + b) / (t - b);
    o.at(2, 3) = -(f + n) / (f - n);
    return o;
  }

  static Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up) {
    const Vec3 f = normalize(target - eye);
    Vec3 s = normalize(cross(f, up));
    if (length(s) < 1e-6f) s = normalize(cross(f, Vec3{0, 0, 1}));
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

  // General 4x4 inverse (identity when singular).
  Mat4 inverse() const {
    const float* a = m;
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    Mat4 r;
    if (std::fabs(det) < 1e-20f) return r;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
    return r;
  }
};

struct Transform {
  Vec3 position;
  Quat rotation;
  Vec3 scale{1.0f, 1.0f, 1.0f};
  Mat4 matrix() const { return Mat4::trs(position, rotation, scale); }
  bool operator==(const Transform& o) const = default;
};

// Axis-aligned box.
struct Aabb {
  Vec3 min{0, 0, 0};
  Vec3 max{0, 0, 0};
  bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
  Vec3 center() const { return (min + max) * 0.5f; }
  Vec3 extent() const { return max - min; }
  // Bounds of this box transformed by m.
  Aabb transformed(const Mat4& m) const {
    Aabb r{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    for (int i = 0; i < 8; ++i) {
      const Vec3 c{(i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y,
                   (i & 4) ? max.z : min.z};
      const Vec3 p = m.transform_point(c);
      r.min = vmin(r.min, p);
      r.max = vmax(r.max, p);
    }
    return r;
  }
};

struct Ray {
  Vec3 origin;
  Vec3 dir;  // normalized
};

// Slab test; distance along the ray to the entry point (or 0 if inside).
inline bool ray_aabb(const Ray& r, const Aabb& b, float* t_out) {
  float t0 = 0.0f, t1 = 1e30f;
  for (int i = 0; i < 3; ++i) {
    const float o = r.origin[i], d = r.dir[i];
    if (std::fabs(d) < 1e-12f) {
      if (o < b.min[i] || o > b.max[i]) return false;
      continue;
    }
    float ta = (b.min[i] - o) / d, tb = (b.max[i] - o) / d;
    if (ta > tb) { const float t = ta; ta = tb; tb = t; }
    if (ta > t0) t0 = ta;
    if (tb < t1) t1 = tb;
    if (t0 > t1) return false;
  }
  if (t_out) *t_out = t0;
  return true;
}

// Ray against an oriented box: `local` bounds under the world matrix.
inline bool ray_obb(const Ray& r, const Aabb& local, const Mat4& world,
                    float* t_out) {
  const Mat4 inv = world.inverse();
  Ray lr{inv.transform_point(r.origin), inv.transform_dir(r.dir)};
  const float len = length(lr.dir);
  if (len <= 0.0f) return false;
  lr.dir = lr.dir * (1.0f / len);
  float t = 0.0f;
  if (!ray_aabb(lr, local, &t)) return false;
  const Vec3 hit = world.transform_point(lr.origin + lr.dir * t);
  if (t_out) *t_out = length(hit - r.origin);
  return true;
}

// Ray against the plane dot(n, p) = d.
inline bool ray_plane(const Ray& r, const Vec3& n, float d, float* t_out) {
  const float den = dot(n, r.dir);
  if (std::fabs(den) < 1e-8f) return false;
  const float t = (d - dot(n, r.origin)) / den;
  if (t < 0.0f) return false;
  if (t_out) *t_out = t;
  return true;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
