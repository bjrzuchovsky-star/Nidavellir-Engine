#include "editor/editor3d/Gizmo3D.h"

#include <cmath>

namespace ts {
namespace tombstone {
namespace editor {

using render3d::Camera3D;
using render3d::Quat;
using render3d::Vec3;

float Gizmo3D::handle_length(const Camera3D& cam, float vh, const Vec3& origin) {
  const float dist = render3d::length(origin - cam.position);
  const float world_per_px =
      2.0f * dist * std::tan(render3d::radians(cam.fov_y_degrees) * 0.5f) / (vh > 1 ? vh : 1);
  return kHandlePixels * world_per_px;
}

namespace {

struct Ctx {
  const Camera3D& cam;
  float vw, vh;
  Vec3 o;
  float len;
  float ox = 0, oy = 0;
  const GizmoHandle& hover;
  std::vector<GizmoShape>* out;

  bool proj(const Vec3& p, float* x, float* y) const { return cam.project(p, vw, vh, x, y); }

  std::uint32_t tint(std::uint32_t c, const GizmoHandle& h) const {
    return h == hover ? gizmo_highlight(c) : c;
  }

  void line(const Vec3& a, const Vec3& b, std::uint32_t c, float th, const GizmoHandle& h) {
    float ax, ay, bx, by;
    if (!proj(a, &ax, &ay) || !proj(b, &bx, &by)) return;
    GizmoShape s;
    s.kind = GizmoShape::Kind::Polyline;
    s.xy = {ax, ay, bx, by};
    s.colour = tint(c, h);
    s.thickness = h == hover && h.any() ? th + 1.5f : th;
    s.handle = h;
    out->push_back(std::move(s));
  }

  void circle(float r_px, bool filled, std::uint32_t c, float th, const GizmoHandle& h) {
    GizmoShape s;
    s.kind = filled ? GizmoShape::Kind::Disc : GizmoShape::Kind::Circle;
    s.cx = ox;
    s.cy = oy;
    s.radius = r_px;
    s.colour = tint(c, h);
    s.thickness = h == hover && h.any() ? th + 1.5f : th;
    s.handle = h;
    out->push_back(std::move(s));
  }

  // Screen direction and pixel length of a world axis from the origin.
  bool screen_axis(const Vec3& dir, float* dx, float* dy, float* px_per_len) const {
    float ex, ey;
    if (!proj(o + dir * len, &ex, &ey)) return false;
    *dx = ex - ox;
    *dy = ey - oy;
    const float l = std::sqrt(*dx * *dx + *dy * *dy);
    if (l < 6.0f) return false;  // pointing at the camera
    *dx /= l;
    *dy /= l;
    *px_per_len = l;
    return true;
  }

  // Shaft from t0 to t1 (handle lengths) with a cone or a cube at the tip.
  void arrow(int a, const Vec3& dir, float t0, float t1, bool cube, GizmoPart part) {
    float dx, dy, pl;
    if (!screen_axis(dir, &dx, &dy, &pl)) return;
    const GizmoHandle h{part, a};
    const std::uint32_t c = tint(gizmo_axis_colour(a), h);
    line(o + dir * (len * t0), o + dir * (len * t1), gizmo_axis_colour(a), 2.5f, h);
    float tx, ty;
    if (!proj(o + dir * (len * t1), &tx, &ty)) return;
    GizmoShape tip;
    tip.kind = GizmoShape::Kind::Polygon;
    tip.colour = c;
    tip.handle = h;
    const float nx = -dy, ny = dx;
    if (cube) {
      const float s = h == hover ? 6.5f : 5.5f;
      tip.xy = {tx + (dx + nx) * s, ty + (dy + ny) * s, tx + (dx - nx) * s, ty + (dy - ny) * s,
                tx + (-dx - nx) * s, ty + (-dy - ny) * s, tx + (-dx + nx) * s, ty + (-dy + ny) * s};
    } else {
      const float w = h == hover ? 7.0f : 6.0f, l = 16.0f;
      tip.xy = {tx + nx * w, ty + ny * w, tx + dx * l, ty + dy * l, tx - nx * w, ty - ny * w};
    }
    out->push_back(std::move(tip));
  }

  // Square in the plane spanned by u and v (normal axis `n`).
  void plane(int n, const Vec3& u, const Vec3& v, float t0, float t1, GizmoPart part) {
    const GizmoHandle h{part, n};
    const Vec3 corners[4] = {o + (u * t0 + v * t0) * len, o + (u * t1 + v * t0) * len,
                             o + (u * t1 + v * t1) * len, o + (u * t0 + v * t1) * len};
    GizmoShape s;
    s.kind = GizmoShape::Kind::Polygon;
    for (const Vec3& p : corners) {
      float x, y;
      if (!proj(p, &x, &y)) return;
      s.xy.push_back(x);
      s.xy.push_back(y);
    }
    // Edge-on squares are slivers: skip them.
    const float ax = s.xy[2] - s.xy[0], ay = s.xy[3] - s.xy[1];
    const float bx = s.xy[6] - s.xy[0], by = s.xy[7] - s.xy[1];
    if (std::fabs(ax * by - ay * bx) < 30.0f) return;
    s.colour = tint(gizmo_axis_colour(n, 150), h);
    s.handle = h;
    out->push_back(std::move(s));
  }

  // Front half of the ring around `axis` (radius r handle lengths).
  void ring(int a, const Vec3& axis, const Vec3& u, const Vec3& v, float r) {
    const GizmoHandle h{GizmoPart::RotateAxis, a};
    const Vec3 to_cam = render3d::normalize(cam.position - o);
    GizmoShape s;
    s.kind = GizmoShape::Kind::Polyline;
    s.colour = tint(gizmo_axis_colour(a), h);
    s.thickness = h == hover ? 4.0f : 2.5f;
    s.handle = h;
    auto flush = [&]() {
      if (s.xy.size() >= 4) out->push_back(s);
      s.xy.clear();
    };
    (void)axis;
    for (int i = 0; i <= 64; ++i) {
      const float t = 2.0f * render3d::kPi * static_cast<float>(i) / 64.0f;
      const Vec3 d = u * std::cos(t) + v * std::sin(t);
      float x, y;
      if (render3d::dot(d, to_cam) < -0.02f || !proj(o + d * (len * r), &x, &y)) {
        flush();
        continue;
      }
      s.xy.push_back(x);
      s.xy.push_back(y);
    }
    flush();
  }
};

}  // namespace

void Gizmo3D::layout(const Camera3D& cam, float vw, float vh, const GizmoFrame& f,
                     const GizmoHandle& hover, std::vector<GizmoShape>* out) const {
  if (mode == GizmoMode::None) return;
  Ctx c{cam, vw, vh, f.origin, handle_length(cam, vh, f.origin), 0, 0, hover, out};
  if (!cam.project(f.origin, vw, vh, &c.ox, &c.oy)) return;
  Vec3 ax[3], sx[3];
  for (int a = 0; a < 3; ++a) {
    ax[a] = f.axes.rotate(axis_dir(a));
    sx[a] = f.scale_axes.rotate(axis_dir(a));
  }
  const float kPx = kHandlePixels;
  const bool move = mode == GizmoMode::Translate, rot = mode == GizmoMode::Rotate,
             scale = mode == GizmoMode::Scale, all = mode == GizmoMode::Transform;
  // Order matters for picking: small, exact handles first.
  if (move || all) c.circle(10.0f, false, kGizmoWhite, 2.0f, {GizmoPart::MoveFree, -1});
  if (move || all) {
    const float t0 = all ? 1.05f : 0.22f, t1 = all ? 1.4f : 1.0f;
    for (int a = 0; a < 3; ++a) c.arrow(a, ax[a], t0, t1, false, GizmoPart::MoveAxis);
  }
  if (scale || all) {
    const float t0 = all ? 0.18f : 0.22f, t1 = all ? 0.55f : 1.0f;
    for (int a = 0; a < 3; ++a) c.arrow(a, sx[a], t0, t1, true, GizmoPart::ScaleAxis);
  }
  if (move) {
    for (int a = 0; a < 3; ++a) c.plane(a, ax[(a + 1) % 3], ax[(a + 2) % 3], 0.3f, 0.48f, GizmoPart::MovePlane);
  }
  if (scale) {
    for (int a = 0; a < 3; ++a) c.plane(a, sx[(a + 1) % 3], sx[(a + 2) % 3], 0.3f, 0.48f, GizmoPart::ScalePlane);
  }
  if (rot || all) {
    const float r = all ? 0.8f : 1.0f;
    for (int a = 0; a < 3; ++a) c.ring(a, ax[a], ax[(a + 1) % 3], ax[(a + 2) % 3], r);
    c.circle(kPx * (all ? 0.95f : 1.2f), false, kGizmoWhite, 2.0f, {GizmoPart::RotateView, -1});
    if (rot) {
      // The ball: faint fill, drag inside it to tumble freely.
      c.circle(kPx * r, true, hover.part == GizmoPart::Trackball ? gizmo_rgba(255, 255, 255, 40)
                                                                 : gizmo_rgba(255, 255, 255, 14),
               1.0f, {GizmoPart::Trackball, -1});
    }
  }
  if (scale || all) {
    c.circle(kPx * (all ? 1.6f : 1.2f), false, kGizmoWhite, 2.0f, {GizmoPart::ScaleUniform, -1});
  }
  // Origin dot (decoration).
  c.circle(3.0f, true, gizmo_rgba(240, 230, 200), 1.0f, {});
}

GizmoHandle Gizmo3D::pick(const Camera3D& cam, float vw, float vh, const GizmoFrame& f, float mx,
                          float my) const {
  std::vector<GizmoShape> shapes;
  layout(cam, vw, vh, f, {}, &shapes);
  return gizmo_pick(shapes, mx, my, kPickPixels);
}

int Gizmo3D::hit(const Camera3D& cam, float vw, float vh, const Vec3& origin, float mx,
                 float my) const {
  GizmoFrame f;
  f.origin = origin;
  const GizmoHandle h = pick(cam, vw, vh, f, mx, my);
  switch (h.part) {
    case GizmoPart::MoveAxis:
    case GizmoPart::RotateAxis:
    case GizmoPart::ScaleAxis: return h.axis;
    default: return -1;
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
