#include "editor/transform/GizmoShapes.h"

#include <cmath>

namespace ts {
namespace tombstone {
namespace editor {

const char* gizmo_mode_label(GizmoMode m) {
  switch (m) {
    case GizmoMode::Translate: return "Move";
    case GizmoMode::Rotate: return "Rotate";
    case GizmoMode::Scale: return "Scale";
    case GizmoMode::Transform: return "Transform";
    case GizmoMode::None: return "Tweak";
  }
  return "";
}

std::uint32_t gizmo_axis_colour(int axis, int alpha) {
  switch (axis) {
    case 0: return gizmo_rgba(255, 51, 82, alpha);
    case 1: return gizmo_rgba(139, 220, 0, alpha);
    default: return gizmo_rgba(40, 144, 255, alpha);
  }
}

std::uint32_t gizmo_highlight(std::uint32_t c) {
  auto mix = [](std::uint32_t v) { return v + (255 - v) / 2; };
  const std::uint32_t r = mix(c & 0xff), g = mix((c >> 8) & 0xff), b = mix((c >> 16) & 0xff);
  std::uint32_t a = (c >> 24) & 0xff;
  a = a < 200 ? a + 55 : 255;
  return r | (g << 8) | (b << 16) | (a << 24);
}

namespace {

float seg_dist(float px, float py, float ax, float ay, float bx, float by) {
  const float dx = bx - ax, dy = by - ay;
  const float l2 = dx * dx + dy * dy;
  float t = l2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0f;
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  const float cx = ax + dx * t - px, cy = ay + dy * t - py;
  return std::sqrt(cx * cx + cy * cy);
}

bool inside_polygon(const std::vector<float>& xy, float px, float py) {
  const std::size_t n = xy.size() / 2;
  bool in = false;
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const float xi = xy[2 * i], yi = xy[2 * i + 1], xj = xy[2 * j], yj = xy[2 * j + 1];
    if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
  }
  return in;
}

float shape_dist(const GizmoShape& s, float mx, float my) {
  switch (s.kind) {
    case GizmoShape::Kind::Polyline: {
      float best = 1e9f;
      for (std::size_t i = 2; i + 1 < s.xy.size(); i += 2) {
        best = std::fmin(best, seg_dist(mx, my, s.xy[i - 2], s.xy[i - 1], s.xy[i], s.xy[i + 1]));
      }
      return best;
    }
    case GizmoShape::Kind::Polygon:
      return s.xy.size() >= 6 && inside_polygon(s.xy, mx, my) ? 0.0f : 1e9f;
    case GizmoShape::Kind::Circle: {
      // Small circles (the free-move centre) grab from inside too.
      const float r = std::hypot(mx - s.cx, my - s.cy);
      if (s.radius <= 16.0f && r <= s.radius) return 0.0f;
      return std::fabs(r - s.radius);
    }
    case GizmoShape::Kind::Disc:
      return std::hypot(mx - s.cx, my - s.cy) <= s.radius ? 0.0f : 1e9f;
  }
  return 1e9f;
}

}  // namespace

GizmoHandle gizmo_pick(const std::vector<GizmoShape>& shapes, float mx, float my, float pick_px) {
  GizmoHandle best;
  float best_d = pick_px;
  // The trackball fills the ball, so it only wins where nothing else does.
  for (int pass = 0; pass < 2 && !best.any(); ++pass) {
    for (const GizmoShape& s : shapes) {
      if (!s.handle.any() || (s.handle.part == GizmoPart::Trackball) != (pass == 1)) continue;
      const float d = shape_dist(s, mx, my);
      if (d < best_d) {
        best_d = d;
        best = s.handle;
      }
    }
  }
  return best;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
