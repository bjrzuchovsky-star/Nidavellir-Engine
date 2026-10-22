#pragma once

// Blender-style gizmo for the 2D viewport (X red, Y green; Y points down
// the screen like world y). Canvas pixels; constant on-screen size.
//   Move       arrows, an XY square, white centre circle (free)
//   Rotate     white ring (turns about the view, i.e. the entity's centre
//              or the pivot)
//   Scale      cube-tipped handles, an XY square, white outer circle
//   Transform  all of them
// A drag becomes a modal transform with that constraint
// (Editor2DScreen_Transform.cpp).

#include <vector>

#include "editor/transform/GizmoShapes.h"

namespace ts {
namespace tombstone {
namespace editor {

struct Gizmo2D {
  static constexpr float kHandlePixels = 80.0f;
  static constexpr float kPickPixels = 8.0f;

  GizmoMode mode = GizmoMode::None;

  // Unit screen direction of axis 0 (X) / 1 (Y) turned by deg (clockwise).
  static void axis_dir(int axis, float deg, float* dx, float* dy);
  // axes_deg: move axes (0 = Global); scale_deg: the active entity's turn.
  void layout(float cx, float cy, float axes_deg, float scale_deg, const GizmoHandle& hover,
              std::vector<GizmoShape>* out) const;
  GizmoHandle pick(float cx, float cy, float axes_deg, float scale_deg, float mx, float my) const;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
