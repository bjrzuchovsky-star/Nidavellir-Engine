#pragma once

// Plain vectors for the AI toolkit (no renderer dependency: the headless
// server runs AI too). 3D uses y up; 2D worlds map (x, y) to (x, 0, y)
// when they share code with 3D (perception, steering).

#include <cmath>

namespace ts {
namespace ai {

struct V3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  V3() = default;
  V3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
  V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  V3 operator*(float s) const { return {x * s, y * s, z * s}; }
  V3& operator+=(const V3& o) {
    x += o.x;
    y += o.y;
    z += o.z;
    return *this;
  }
  bool operator==(const V3& o) const = default;
};

inline float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(const V3& v) { return std::sqrt(dot(v, v)); }
inline float length_xz(const V3& v) { return std::sqrt(v.x * v.x + v.z * v.z); }
inline float distance(const V3& a, const V3& b) { return length(a - b); }
inline float distance_xz(const V3& a, const V3& b) { return length_xz(a - b); }
inline V3 normalized(const V3& v) {
  const float l = length(v);
  return l > 1e-6f ? v * (1.0f / l) : V3{};
}
inline V3 flat(const V3& v) { return {v.x, 0.0f, v.z}; }
inline V3 truncate(const V3& v, float max_len) {
  const float l = length(v);
  return l > max_len && l > 1e-6f ? v * (max_len / l) : v;
}

}  // namespace ai
}  // namespace ts
