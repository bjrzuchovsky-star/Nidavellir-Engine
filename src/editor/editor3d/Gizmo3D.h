#pragma once

// Translate / rotate / scale gizmo math for the 3D viewport, in screen
// space (pixels, 0,0 = top-left of the viewport). Pure functions, so the
// editor UI and --smoke drive the same code:
//
//   Gizmo3D g;  g.mode = GizmoMode::Translate;
//   int axis = g.hit(cam, w, h, origin, mouse);         // -1 = miss
//   g.begin(axis, transform, mouse);
//   Transform t = g.drag(cam, w, h, mouse, snap ? &steps : nullptr);
//
// Handles are sized to stay ~kHandlePixels long on screen whatever the
// distance. Axes are world X / Y / Z.

#include "render3d/Camera3D.h"

namespace ts {
namespace tombstone {
namespace editor {

enum class GizmoMode { Translate, Rotate, Scale };

struct GizmoSnap {
  float translate = 0.5f;  // world units
  float rotate = 15.0f;    // degrees
  float scale = 0.25f;     // scale units
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

  // Which handle the mouse is on (0 x, 1 y, 2 z) or -1.
  int hit(const render3d::Camera3D& cam, float vw, float vh,
          const render3d::Vec3& origin, float mx, float my) const;

  void begin(int axis, const render3d::Transform& start, const render3d::Camera3D& cam,
             float vw, float vh, float mx, float my);
  bool active() const { return axis_ >= 0; }
  int axis() const { return axis_; }
  void cancel() { axis_ = -1; }
  // Current transform for the mouse at (mx, my).
  render3d::Transform drag(const render3d::Camera3D& cam, float vw, float vh, float mx,
                           float my, const GizmoSnap* snap) const;
  const render3d::Transform& start() const { return start_; }

  // Ring / handle points for drawing (screen space); false when behind.
  static bool screen(const render3d::Camera3D& cam, float vw, float vh,
                     const render3d::Vec3& p, float* sx, float* sy) {
    return cam.project(p, vw, vh, sx, sy);
  }

 private:
  int axis_ = -1;
  render3d::Transform start_;
  float start_mx_ = 0.0f, start_my_ = 0.0f;
  float start_angle_ = 0.0f;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
