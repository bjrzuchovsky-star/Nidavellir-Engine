#include "editor/transform/ModalKeys.h"

#include <imgui.h>

#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace {
bool pressed(ImGuiKey k, bool repeat = false) { return ImGui::IsKeyPressed(k, repeat); }
}  // namespace


void draw_gizmo_shapes(ImDrawList* dl, const std::vector<GizmoShape>& shapes, float x0, float y0) {
  for (const GizmoShape& s : shapes) {
    switch (s.kind) {
      case GizmoShape::Kind::Polyline: {
        std::vector<ImVec2> pts;
        for (std::size_t i = 0; i + 1 < s.xy.size(); i += 2) pts.emplace_back(x0 + s.xy[i], y0 + s.xy[i + 1]);
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), s.colour, ImDrawFlags_None, s.thickness);
        break;
      }
      case GizmoShape::Kind::Polygon: {
        std::vector<ImVec2> pts;
        for (std::size_t i = 0; i + 1 < s.xy.size(); i += 2) pts.emplace_back(x0 + s.xy[i], y0 + s.xy[i + 1]);
        dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), s.colour);
        break;
      }
      case GizmoShape::Kind::Circle:
        dl->AddCircle(ImVec2(x0 + s.cx, y0 + s.cy), s.radius, s.colour, 64, s.thickness);
        break;
      case GizmoShape::Kind::Disc:
        dl->AddCircleFilled(ImVec2(x0 + s.cx, y0 + s.cy), s.radius, s.colour, 48);
        break;
    }
  }
}


ModalKeys poll_modal_keys(ModalTransform& m) {
  ModalKeys out;
  const ImGuiIO& io = ImGui::GetIO();
  if (pressed(ImGuiKey_Escape)) out.cancel = true;
  if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter) || pressed(ImGuiKey_Space)) {
    out.confirm = true;
  }
  if (io.KeyCtrl || io.KeyAlt) return out;  // Ctrl = snap toggle while held
  const ImGuiKey axes[3] = {ImGuiKey_X, ImGuiKey_Y, ImGuiKey_Z};
  for (int a = 0; a < m.dims(); ++a) {
    if (pressed(axes[a])) m.press_axis(a, io.KeyShift);
  }
  for (int d = 0; d <= 9; ++d) {
    if (pressed(static_cast<ImGuiKey>(ImGuiKey_0 + d)) ||
        pressed(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + d))) {
      m.type_char(static_cast<char>('0' + d));
    }
  }
  if (pressed(ImGuiKey_Minus) || pressed(ImGuiKey_KeypadSubtract)) m.type_char('-');
  if (pressed(ImGuiKey_Period) || pressed(ImGuiKey_KeypadDecimal)) m.type_char('.');
  if (pressed(ImGuiKey_Backspace, true)) m.backspace();
  if (pressed(ImGuiKey_Tab)) m.next_component();
  if (pressed(ImGuiKey_G)) out.switch_to = XformOp::Move;
  if (pressed(ImGuiKey_S)) out.switch_to = XformOp::Scale;
  if (pressed(ImGuiKey_R)) {
    if (m.op() == XformOp::Rotate && m.dims() == 3) out.trackball = true;
    else out.switch_to = XformOp::Rotate;
  }
  return out;
}

void modal_keyboard_nav(bool transform_running) {
  if (!ImGui::GetCurrentContext()) return;
  static bool saved = false;
  static bool had_nav = false;
  ImGuiIO& io = ImGui::GetIO();
  if (transform_running && !saved) {
    had_nav = (io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0;
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    saved = true;
  } else if (!transform_running && saved) {
    if (had_nav) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    saved = false;
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
