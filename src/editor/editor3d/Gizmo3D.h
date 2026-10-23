#pragma once

// Blender-style transform gizmo for the 3D viewport (docs/transforms.md),
// laid out in screen space (pixels, 0,0 = top-left of the viewport):
//
//   Move       arrows per axis, plane squares, white centre circle (free)
//   Rotate     a ring per axis (front half), white view ring, trackball
//              inside the ball
//   Scale      cube-tipped handles, plane squares, white outer circle
//              (uniform)
//   Transform  all three at once
//
// X / Y / Z are red / green / blue. Handles stay ~kHandlePixels long on
// screen whatever the distance. Move / rotate axes follow the
// orientation (Global = world, Local = the active entity); scale handles
// always use the entity's own axes. A drag on a handle becomes a modal
// transform with that constraint (Editor3DScreen_Transform.cpp), so snap,
// precision, pivots and undo behave the same as G / R / S.

#include <vector>

#include "editor/transform/GizmoShapes.h"
#include "render3d/Camera3D.h"

namespace ts {
namespace tombstone {
namespace editor {

struct GizmoSnap {
  float translate = 0.5f;  // world units
  float rotate = 15.0f;    // degrees
  float scale = 0.25f;     // scale units
};

// Where the gizmo sits and how it is turned.
struct GizmoFrame {
  render3d::Vec3 origin{0, 0, 0};
  render3d::Quat axes{0, 0, 0, 1};        // move / rotate axes
  render3d::Quat scale_axes{0, 0, 0, 1};  // scale handles (entity axes)
};

struct Gizmo3D {
  static constexpr float kHandlePixels = 90.0f;
  static constexpr float kPickPixels = 9.0f;

  GizmoMode mode = GizmoMode::Translate;

  // World length of a handle at `origin`.
  static float handle_length(const render3d::Camera3D& cam, float vh,
                             const render3d::Vec3& origin);
  static render3d::Vec3 axis_dir(int axis) {
    return axis == 0 ? render3d::Vec3{1, 0, 0}
                     : (axis == 1 ? render3d::Vec3{0, 1, 0} : render3d::Vec3{0, 0, 1});
  }

  // Shapes for the current mode; `hover` is drawn highlighted.
  void layout(const render3d::Camera3D& cam, float vw, float vh, const GizmoFrame& f,
              const GizmoHandle& hover, std::vector<GizmoShape>* out) const;
  // Handle under the mouse (part None on a miss).
  GizmoHandle pick(const render3d::Camera3D& cam, float vw, float vh, const GizmoFrame& f,
                   float mx, float my) const;
  // World-axis frame at `origin`: the axis (0-2) of the handle under the
  // mouse, or -1 (kept for older callers).
  int hit(const render3d::Camera3D& cam, float vw, float vh, const render3d::Vec3& origin,
          float mx, float my) const;

  // The handle being dragged.
  void set_drag(const GizmoHandle& h) { drag_ = h; }
  const GizmoHandle& drag() const { return drag_; }
  bool active() const { return drag_.any(); }
  int axis() const { return drag_.axis; }
  void cancel() { drag_ = GizmoHandle{}; }

 private:
  GizmoHandle drag_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
