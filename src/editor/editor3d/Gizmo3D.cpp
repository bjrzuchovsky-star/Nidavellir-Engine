#include "editor/editor3d/Gizmo3D.h"

#include <cmath>

namespace ts {
namespace tombstone {
namespace editor {

using render3d::Camera3D;
using render3d::Quat;
using render3d::Transform;
using render3d::Vec3;

namespace {

float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
  const float dx = bx - ax, dy = by - ay;
  const float l2 = dx * dx + dy * dy;
  float t = l2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0f;
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  const float cx = ax + dx * t - px, cy = ay + dy * t - py;
  return std::sqrt(cx * cx + cy * cy);
}

void ring_basis(int axis, Vec3* u, Vec3* v) {
  if (axis == 0) {
    *u = {0, 1, 0};
    *v = {0, 0, 1};
  } else if (axis == 1) {
    *u = {0, 0, 1};
    *v = {1, 0, 0};
  } else {
    *u = {1, 0, 0};
    *v = {0, 1, 0};
  }
}

float snap_to(float v, float step) {
  return step > 0.0f ? std::round(v / step) * step : v;
}

}  // namespace

float Gizmo3D::handle_length(const Camera3D& cam, float vh, const Vec3& origin) {
  const float dist = render3d::length(origin - cam.position);
  const float world_per_px =
      2.0f * dist * std::tan(render3d::radians(cam.fov_y_degrees) * 0.5f) / (vh > 1 ? vh : 1);
  return kHandlePixels * world_per_px;
}

int Gizmo3D::hit(const Camera3D& cam, float vw, float vh, const Vec3& o, float mx,
                 float my) const {
  const float len = handle_length(cam, vh, o);
  float ox = 0, oy = 0;
  if (!cam.project(o, vw, vh, &ox, &oy)) return -1;
  int best = -1;
  float best_d = kPickPixels;
  for (int a = 0; a < 3; ++a) {
    if (mode == GizmoMode::Rotate) {
      Vec3 u, v;
      ring_basis(a, &u, &v);
      float px = 0, py = 0;
      bool have = false;
      for (int i = 0; i <= 48; ++i) {
        const float th = render3d::kPi * 2.0f * i / 48.0f;
        float sx = 0, sy = 0;
        if (!cam.project(o + (u * std::cos(th) + v * std::sin(th)) * len, vw, vh, &sx, &sy)) {
          have = false;
          continue;
        }
        if (have) {
          const float d = seg_dist(mx, my, px, py, sx, sy);
          if (d < best_d) {
            best_d = d;
            best = a;
          }
        }
        px = sx;
        py = sy;
        have = true;
      }
    } else {
      float ex = 0, ey = 0;
      if (!cam.project(o + axis_dir(a) * len, vw, vh, &ex, &ey)) continue;
      const float d = seg_dist(mx, my, ox, oy, ex, ey);
      if (d < best_d) {
        best_d = d;
        best = a;
      }
    }
  }
  return best;
}

void Gizmo3D::begin(int axis, const Transform& start, const Camera3D& cam, float vw, float vh,
                    float mx, float my) {
  axis_ = axis;
  start_ = start;
  start_mx_ = mx;
  start_my_ = my;
  float ox = 0, oy = 0;
  cam.project(start.position, vw, vh, &ox, &oy);
  start_angle_ = std::atan2(my - oy, mx - ox);
}

Transform Gizmo3D::drag(const Camera3D& cam, float vw, float vh, float mx, float my,
                        const GizmoSnap* snap) const {
  Transform t = start_;
  if (axis_ < 0) return t;
  const Vec3 o = start_.position;
  const Vec3 dir = axis_dir(axis_);
  const float len = handle_length(cam, vh, o);
  float ox = 0, oy = 0, ex = 0, ey = 0;
  if (!cam.project(o, vw, vh, &ox, &oy)) return t;
  if (mode == GizmoMode::Rotate) {
    float a = std::atan2(my - oy, mx - ox) - start_angle_;
    while (a > render3d::kPi) a -= 2.0f * render3d::kPi;
    while (a < -render3d::kPi) a += 2.0f * render3d::kPi;
    // Screen y grows down, so a clockwise-looking drag is negative; flip
    // when the axis points away from the viewer.
    float deg = -render3d::degrees(a);
    if (render3d::dot(dir, cam.position - o) < 0.0f) deg = -deg;
    if (snap) deg = snap_to(deg, snap->rotate);
    t.rotation = render3d::normalize(Quat::axis_angle(dir, render3d::radians(deg)) * start_.rotation);
    return t;
  }
  if (!cam.project(o + dir * len, vw, vh, &ex, &ey)) return t;
  float sdx = ex - ox, sdy = ey - oy;
  const float px_len = std::sqrt(sdx * sdx + sdy * sdy);
  if (px_len < 1.0f) return t;  // axis points at the camera
  sdx /= px_len;
  sdy /= px_len;
  const float moved_px = (mx - start_mx_) * sdx + (my - start_my_) * sdy;
  const float world = moved_px * (len / px_len);
  if (mode == GizmoMode::Translate) {
    t.position[axis_] = start_.position[axis_] + world;
    if (snap) t.position[axis_] = snap_to(t.position[axis_], snap->translate);
  } else {
    float s = start_.scale[axis_] * (1.0f + world / len);
    if (snap) s = snap_to(s, snap->scale);
    const float min_s = snap && snap->scale > 0 ? snap->scale : 0.01f;
    t.scale[axis_] = s < min_s ? min_s : s;
  }
  return t;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
