#pragma once

// Keyboard side of a modal transform (ImGui). The editors call
// poll_modal_keys() each frame while a G / R / S transform runs and skip
// their own shortcuts, so X / Y / Z, digits, Enter and Esc go only to the
// transform (Delete, Ctrl+X and the tool keys are untouched otherwise).

#include "editor/transform/GizmoShapes.h"
#include "editor/transform/ModalTransform.h"

#include <vector>

struct ImDrawList;

namespace ts {
namespace tombstone {
namespace editor {

struct ModalKeys {
  bool confirm = false;               // Enter / keypad Enter / Space
  bool cancel = false;                // Esc
  XformOp switch_to = XformOp::None;  // G / R / S pressed again
  bool trackball = false;             // R during a 3D rotate
};

// Feeds axis keys and typed numbers into `m`; returns the rest.
ModalKeys poll_modal_keys(ModalTransform& m);

// Keyboard navigation off while a transform runs (Tab / Enter belong to
// it), back on afterwards. Safe without an ImGui context.
void modal_keyboard_nav(bool transform_running);

// Draw gizmo shapes (canvas pixels) offset by the canvas corner.
void draw_gizmo_shapes(ImDrawList* dl, const std::vector<GizmoShape>& shapes, float x0, float y0);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
