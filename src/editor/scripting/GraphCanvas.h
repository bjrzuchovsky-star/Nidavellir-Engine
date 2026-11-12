#pragma once

// Shared node-canvas chrome for the Brand Book and the Animator graph:
// the same grid, card, and wire. Callers still own hit testing and data.

#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {
namespace graphcanvas {

inline constexpr float kNodeW = 196.0f;
inline constexpr float kHeaderH = 30.0f;
inline constexpr float kRowH = 21.0f;
inline constexpr float kPinR = 5.5f;

inline void draw_grid(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, float pan_x, float pan_y, float zoom) {
  dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
  dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), theme::U32(theme::CanvasBg()));
  const float step = 24.0f * zoom;
  if (step < 4.0f) return;
  for (float x = std::fmod(pan_x, step); x < size.x; x += step) {
    const bool major = static_cast<int>(std::round((x - pan_x) / step)) % 5 == 0;
    dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y),
                theme::U32(theme::GridLine(), major ? 1.6f : 0.7f));
  }
  for (float y = std::fmod(pan_y, step); y < size.y; y += step) {
    const bool major = static_cast<int>(std::round((y - pan_y) / step)) % 5 == 0;
    dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y),
                theme::U32(theme::GridLine(), major ? 1.6f : 0.7f));
  }
}

inline void draw_wire(ImDrawList* dl, ImVec2 from, ImVec2 to, ImU32 color, float thickness) {
  const float dx = std::max(40.0f, std::fabs(to.x - from.x) * 0.5f);
  dl->AddBezierCubic(from, ImVec2(from.x + dx, from.y), ImVec2(to.x - dx, to.y), to, color, thickness);
}

inline void draw_node_card(ImDrawList* dl, ImVec2 a, ImVec2 b, float zoom, const char* title, const char* category,
                           ImVec4 header, bool selected) {
  const float z = zoom;
  const float r = 6.0f * z;
  dl->AddRectFilled(ImVec2(a.x + 4 * z, a.y + 5 * z), ImVec2(b.x + 4 * z, b.y + 5 * z), IM_COL32(0, 0, 0, 70), r);
  dl->AddRectFilled(a, b, theme::U32(theme::PanelBg()), r);
  dl->AddRectFilled(a, ImVec2(b.x, a.y + kHeaderH * z), theme::U32(header), r, ImDrawFlags_RoundCornersTop);
  dl->AddRectFilled(ImVec2(a.x, a.y + kHeaderH * z - 3 * z), ImVec2(b.x, a.y + kHeaderH * z),
                    theme::U32(theme::Charcoal(), 0.35f));
  dl->AddRect(a, b, selected ? theme::U32(theme::Accent()) : theme::U32(theme::Border()), r, 0, selected ? 2.5f : 1.0f);
  theme::DrawSizedText(dl, ImVec2(a.x + 9 * z, a.y + 7 * z), 15.0f * z, theme::U32(theme::TextOnAccent()), title);
  if (category && category[0]) {
    std::string fam = category;
    for (char& c : fam) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const ImVec2 fl = theme::MeasureText(9.5f * z, fam.c_str());
    theme::DrawSizedText(dl, ImVec2(b.x - fl.x - 8 * z, a.y + 10 * z), 9.5f * z,
                         theme::U32(theme::TextOnAccent(), 0.65f), fam.c_str());
  }
}

}  // namespace graphcanvas
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
