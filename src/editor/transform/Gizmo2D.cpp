#include "editor/transform/Gizmo2D.h"

#include <cmath>

namespace ts {
namespace tombstone {
namespace editor {

void Gizmo2D::axis_dir(int axis, float deg, float* dx, float* dy) {
  const float a = deg * 3.14159265358979f / 180.0f;
  const float c = std::cos(a), s = std::sin(a);
  if (axis == 0) {
    *dx = c;
    *dy = s;
  } else {
    *dx = -s;
    *dy = c;
  }
}

namespace {

GizmoShape poly(std::vector<float> xy, std::uint32_t c, const GizmoHandle& h) {
  GizmoShape s;
  s.kind = GizmoShape::Kind::Polygon;
  s.xy = std::move(xy);
  s.colour = c;
  s.handle = h;
  return s;
}

}  // namespace

void Gizmo2D::layout(float cx, float cy, float axes_deg, float scale_deg, const GizmoHandle& hover,
                     std::vector<GizmoShape>* out) const {
  if (mode == GizmoMode::None) return;
  const float R = kHandlePixels;
  auto tint = [&](std::uint32_t c, const GizmoHandle& h) { return h == hover ? gizmo_highlight(c) : c; };
  auto circle = [&](float r, bool filled, std::uint32_t c, const GizmoHandle& h) {
    GizmoShape s;
    s.kind = filled ? GizmoShape::Kind::Disc : GizmoShape::Kind::Circle;
    s.cx = cx;
    s.cy = cy;
    s.radius = r;
    s.colour = tint(c, h);
    s.thickness = h == hover && h.any() ? 3.5f : 2.0f;
    s.handle = h;
    out->push_back(std::move(s));
  };
  auto arrow = [&](int a, float deg, float t0, float t1, bool cube, GizmoPart part) {
    float dx, dy;
    axis_dir(a, deg, &dx, &dy);
    const GizmoHandle h{part, a};
    const std::uint32_t c = tint(gizmo_axis_colour(a), h);
    GizmoShape shaft;
    shaft.kind = GizmoShape::Kind::Polyline;
    shaft.xy = {cx + dx * R * t0, cy + dy * R * t0, cx + dx * R * t1, cy + dy * R * t1};
    shaft.colour = c;
    shaft.thickness = h == hover ? 4.0f : 2.5f;
    shaft.handle = h;
    out->push_back(std::move(shaft));
    const float tx = cx + dx * R * t1, ty = cy + dy * R * t1, nx = -dy, ny = dx;
    if (cube) {
      const float s = h == hover ? 6.5f : 5.5f;
      out->push_back(poly({tx + (dx + nx) * s, ty + (dy + ny) * s, tx + (dx - nx) * s, ty + (dy - ny) * s,
                           tx + (-dx - nx) * s, ty + (-dy - ny) * s, tx + (-dx + nx) * s, ty + (-dy + ny) * s},
                          c, h));
    } else {
      const float w = h == hover ? 7.0f : 6.0f, l = 15.0f;
      out->push_back(poly({tx + nx * w, ty + ny * w, tx + dx * l, ty + dy * l, tx - nx * w, ty - ny * w}, c, h));
    }
  };
  auto square = [&](float deg, float t0, float t1, GizmoPart part) {
    float ux, uy, vx, vy;
    axis_dir(0, deg, &ux, &uy);
    axis_dir(1, deg, &vx, &vy);
    const GizmoHandle h{part, 2};  // the plane with normal Z: X and Y
    auto pt = [&](float a, float b, std::vector<float>* xy) {
      xy->push_back(cx + (ux * a + vx * b) * R);
      xy->push_back(cy + (uy * a + vy * b) * R);
    };
    std::vector<float> xy;
    pt(t0, t0, &xy);
    pt(t1, t0, &xy);
    pt(t1, t1, &xy);
    pt(t0, t1, &xy);
    out->push_back(poly(std::move(xy), tint(gizmo_axis_colour(2, 150), h), h));
  };
  const bool move = mode == GizmoMode::Translate, rot = mode == GizmoMode::Rotate,
             scale = mode == GizmoMode::Scale, all = mode == GizmoMode::Transform;
  if (move || all) circle(10.0f, false, kGizmoWhite, {GizmoPart::MoveFree, -1});
  if (move || all) {
    for (int a = 0; a < 2; ++a) arrow(a, axes_deg, all ? 1.05f : 0.25f, all ? 1.4f : 1.0f, false, GizmoPart::MoveAxis);
  }
  if (scale || all) {
    for (int a = 0; a < 2; ++a) arrow(a, scale_deg, all ? 0.2f : 0.25f, all ? 0.6f : 1.0f, true, GizmoPart::ScaleAxis);
  }
  if (move) square(axes_deg, 0.3f, 0.5f, GizmoPart::MovePlane);
  if (scale) square(scale_deg, 0.3f, 0.5f, GizmoPart::ScalePlane);
  if (rot || all) circle(R * (all ? 0.85f : 1.0f), false, kGizmoWhite, {GizmoPart::RotateView, -1});
  if (scale || all) circle(R * (all ? 1.6f : 1.25f), false, kGizmoWhite, {GizmoPart::ScaleUniform, -1});
  circle(3.0f, true, gizmo_rgba(240, 230, 200), {});
}

GizmoHandle Gizmo2D::pick(float cx, float cy, float axes_deg, float scale_deg, float mx, float my) const {
  std::vector<GizmoShape> shapes;
  layout(cx, cy, axes_deg, scale_deg, {}, &shapes);
  return gizmo_pick(shapes, mx, my, kPickPixels);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
