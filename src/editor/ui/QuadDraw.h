#pragma once

// Turned quads for ImGui draw lists: the 2D edit, Play and posse viewports
// draw entities with Entity2D::rotation / DrawQuad::rotation through these
// (clockwise on screen, about the rect centre, same as ts_game).

#include <imgui.h>

#include <cmath>

namespace ts {
namespace tombstone {
namespace editor {
namespace quad_draw {

// Corners of the screen rect p0..p1 turned by deg: TL, TR, BR, BL.
inline void corners(const ImVec2& p0, const ImVec2& p1, float deg, ImVec2 out[4]) {
  const float cx = (p0.x + p1.x) * 0.5f, cy = (p0.y + p1.y) * 0.5f;
  const float a = deg * 3.14159265f / 180.0f;
  const float c = std::cos(a), s = std::sin(a);
  const ImVec2 local[4] = {ImVec2(p0.x - cx, p0.y - cy), ImVec2(p1.x - cx, p0.y - cy),
                           ImVec2(p1.x - cx, p1.y - cy), ImVec2(p0.x - cx, p1.y - cy)};
  for (int i = 0; i < 4; ++i) {
    out[i] = ImVec2(cx + local[i].x * c - local[i].y * s, cy + local[i].x * s + local[i].y * c);
  }
}

inline void filled(ImDrawList* d, const ImVec2& p0, const ImVec2& p1, float deg, ImU32 col) {
  if (deg == 0.0f) {
    d->AddRectFilled(p0, p1, col);
    return;
  }
  ImVec2 q[4];
  corners(p0, p1, deg, q);
  d->AddQuadFilled(q[0], q[1], q[2], q[3], col);
}

inline void outline(ImDrawList* d, const ImVec2& p0, const ImVec2& p1, float deg, ImU32 col,
                    float thickness) {
  if (deg == 0.0f) {
    d->AddRect(p0, p1, col, 1.0f, 0, thickness);
    return;
  }
  ImVec2 q[4];
  corners(p0, p1, deg, q);
  d->AddQuad(q[0], q[1], q[2], q[3], col, thickness);
}

inline void image(ImDrawList* d, ImTextureID tex, const ImVec2& p0, const ImVec2& p1, float deg,
                  const ImVec2& uv0, const ImVec2& uv1, ImU32 col) {
  if (deg == 0.0f) {
    d->AddImage(tex, p0, p1, uv0, uv1, col);
    return;
  }
  ImVec2 q[4];
  corners(p0, p1, deg, q);
  d->AddImageQuad(tex, q[0], q[1], q[2], q[3], uv0, ImVec2(uv1.x, uv0.y), uv1,
                  ImVec2(uv0.x, uv1.y), col);
}

}  // namespace quad_draw
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
