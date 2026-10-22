#pragma once

// Gizmo handles as flat screen shapes, shared by the 3D gizmo (Gizmo3D)
// and the 2D one (Gizmo2D). Each layout() fills a list the viewport draws
// with ImDrawList and pick() tests the mouse against, so what you see is
// what you grab. Pure (no ImGui), so --smoke-transform can pick handles.

#include <cstdint>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

// Gizmo shown on the selection. None = Tweak: no gizmo (2D: the box
// handles), drag to move, G / R / S still work.
enum class GizmoMode { Translate, Rotate, Scale, Transform, None };
const char* gizmo_mode_label(GizmoMode m);  // "Move", "Rotate", ...

enum class GizmoPart {
  None,
  MoveAxis,      // arrow along `axis`
  MovePlane,     // square; moves in the plane whose normal is `axis`
  MoveFree,      // white centre circle (view plane)
  RotateAxis,    // ring around `axis`
  RotateView,    // white outer ring (around the view direction)
  Trackball,     // inside the rotate ball (3D)
  ScaleAxis,     // cube-tipped handle along `axis`
  ScalePlane,    // square; scales the two axes other than `axis`
  ScaleUniform,  // white outer circle
};

struct GizmoHandle {
  GizmoPart part = GizmoPart::None;
  int axis = -1;
  bool any() const { return part != GizmoPart::None; }
  bool operator==(const GizmoHandle& o) const { return part == o.part && axis == o.axis; }
};

struct GizmoShape {
  enum class Kind { Polyline, Polygon, Circle, Disc };
  Kind kind = Kind::Polyline;
  std::vector<float> xy;  // screen points (Polyline / Polygon)
  float cx = 0, cy = 0, radius = 0;  // Circle / Disc
  std::uint32_t colour = 0xffffffffu;  // IM_COL32 packing (ABGR)
  float thickness = 2.0f;
  GizmoHandle handle;  // part None = decoration only
};

// IM_COL32-compatible packing.
constexpr std::uint32_t gizmo_rgba(int r, int g, int b, int a = 255) {
  return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
         (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(a) << 24);
}
// Blender's axis colours: X red, Y green, Z blue (alpha 0-255).
std::uint32_t gizmo_axis_colour(int axis, int alpha = 255);
inline constexpr std::uint32_t kGizmoWhite = gizmo_rgba(235, 235, 235, 230);
// Lighter, for the handle under the mouse.
std::uint32_t gizmo_highlight(std::uint32_t c);

// Handle under (mx, my): the closest shape within `pick_px` (inside for
// filled shapes); ties go to the earlier shape. Trackball shapes only
// count where nothing else is hit.
GizmoHandle gizmo_pick(const std::vector<GizmoShape>& shapes, float mx, float my, float pick_px);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
