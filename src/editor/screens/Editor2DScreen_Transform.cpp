// Modal transforms (G / R / S), the Blender-style 2D gizmo, pivots, the 2D
// cursor and Alt+G / R / S for the 2D editor. Same rules as the 3D editor
// (docs/transforms.md): a gizmo drag is a modal transform with that
// handle's constraint, one undo step per confirm, Esc / RMB restores.

#include "editor/screens/Editor2DScreen.h"
#include "editor/ux/Tooltip.h"
#include "editor/ux/Actions.h"
#include "editor/transform/ModalKeys.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

constexpr float kRad = 3.14159265358979f / 180.0f;
constexpr float kRotateSnapDeg = 15.0f;
constexpr float kScaleSnapStep = 0.1f;

std::string fmt(float v, const char* f) {
  char b[48];
  std::snprintf(b, sizeof(b), f, v);
  return b;
}

using Op = Workspace2D::Xform2D::Op;

}  // namespace

// ---- Camera helpers ----------------------------------------------------------------------

void Editor2DScreen::canvas_to_world(float px, float py, float* wx, float* wy) const {
  const float z = std::max(workspace_.zoom(), 0.0001f);
  *wx = (px - viewport_w_ * 0.5f) / z + workspace_.pan_x();
  *wy = (py - viewport_h_ * 0.5f) / z + workspace_.pan_y();
}

void Editor2DScreen::world_to_canvas(float wx, float wy, float* px, float* py) const {
  *px = viewport_w_ * 0.5f + (wx - workspace_.pan_x()) * workspace_.zoom();
  *py = viewport_h_ * 0.5f + (wy - workspace_.pan_y()) * workspace_.zoom();
}

// ---- Pivot + gizmo frame -----------------------------------------------------------------

void Editor2DScreen::pivot_point(float* wx, float* wy) const {
  if (modal_active()) {
    *wx = modal_.pivot_x;
    *wy = modal_.pivot_y;
    return;
  }
  *wx = 0.0f;
  *wy = 0.0f;
  std::vector<const Entity2D*> roots;
  for (std::uint64_t id : workspace_.selection_roots()) {
    if (const Entity2D* e = workspace_.find(id)) {
      if (!workspace_.effectively_locked(*e)) roots.push_back(e);
    }
  }
  const Entity2D* prim = workspace_.selected();
  switch (pivot_mode_) {
    case PivotMode::Cursor:
      *wx = cursor2d_x_;
      *wy = cursor2d_y_;
      return;
    case PivotMode::Active:
      if (prim) {
        *wx = prim->center_x();
        *wy = prim->center_y();
        return;
      }
      break;
    case PivotMode::Bounds: {
      float x0, y0, x1, y1;
      if (workspace_.selection_bounds(&x0, &y0, &x1, &y1)) {
        *wx = (x0 + x1) * 0.5f;
        *wy = (y0 + y1) * 0.5f;
        return;
      }
      break;
    }
    case PivotMode::Median:
    case PivotMode::Individual:
      break;
  }
  if (roots.empty() && prim) roots.push_back(prim);
  if (roots.empty()) return;
  for (const Entity2D* e : roots) {
    *wx += e->center_x();
    *wy += e->center_y();
  }
  *wx /= static_cast<float>(roots.size());
  *wy /= static_cast<float>(roots.size());
}

void Editor2DScreen::gizmo_frame(float* cx, float* cy, float* axes_deg, float* scale_deg) const {
  float wx, wy;
  pivot_point(&wx, &wy);
  world_to_canvas(wx, wy, cx, cy);
  const Entity2D* prim = workspace_.selected();
  const float turn = prim ? prim->rotation : 0.0f;
  *axes_deg = orientation_ == XformSpace::Local ? turn : 0.0f;
  *scale_deg = turn;
}

GizmoHandle Editor2DScreen::gizmo_pick(float px, float py) const {
  const Entity2D* prim = workspace_.selected();
  if (!prim || gizmo_.mode == GizmoMode::None || is_playing()) return {};
  float cx, cy, ad, sd;
  gizmo_frame(&cx, &cy, &ad, &sd);
  return gizmo_.pick(cx, cy, ad, sd, px, py);
}

// ---- Modal session ------------------------------------------------------------------------

bool Editor2DScreen::modal_begin(XformOp op, float px, float py) {
  if (is_playing() || op == XformOp::None || modal_active()) return false;
  cancel_rename();
  flush_pending_edit();
  float pivot_x, pivot_y;
  pivot_point(&pivot_x, &pivot_y);  // before the session (pivot_point reads it)
  Workspace2D::Snapshot before = workspace_.snapshot();
  if (!workspace_.begin_xform()) {
    note("Nothing to transform (empty or locked selection)");
    return false;
  }
  modal_ = ModalState{};
  modal_.before = std::move(before);
  modal_.pivot_x = pivot_x;
  modal_.pivot_y = pivot_y;
  // Active root: the root holding the primary selection.
  const auto& roots = workspace_.xform_root_starts();
  if (const Entity2D* p = workspace_.selected()) {
    const Entity2D* walk = p;
    for (int guard = 0; walk && guard < 4096; ++guard) {
      bool found = false;
      for (std::size_t i = 0; i < roots.size(); ++i) {
        if (roots[i].id == walk->id) {
          modal_.active = i;
          found = true;
          break;
        }
      }
      if (found || walk->parent == 0) break;
      walk = workspace_.find(walk->parent);
    }
  }
  modal_.m.begin(op, 2, op == XformOp::Scale ? XformSpace::Local : orientation_, px, py);
  modal_.mx = px;
  modal_.my = py;
  modal_.snap = workspace_.snap_enabled();
  modal_keyboard_nav(true);
  modal_apply();
  return true;
}

bool Editor2DScreen::modal_begin_handle(const GizmoHandle& h, float px, float py) {
  XformOp op = XformOp::None;
  AxisLock lock;
  switch (h.part) {
    case GizmoPart::MoveAxis: op = XformOp::Move; lock.axis = h.axis; break;
    case GizmoPart::MovePlane:
    case GizmoPart::MoveFree: op = XformOp::Move; break;
    case GizmoPart::RotateAxis:
    case GizmoPart::RotateView:
    case GizmoPart::Trackball: op = XformOp::Rotate; break;
    case GizmoPart::ScaleAxis: op = XformOp::Scale; lock.axis = h.axis; break;
    case GizmoPart::ScalePlane:
    case GizmoPart::ScaleUniform: op = XformOp::Scale; break;
    case GizmoPart::None: return false;
  }
  if (!modal_begin(op, px, py)) return false;
  if (lock.axis >= 0) {
    lock.space = op == XformOp::Scale ? XformSpace::Local : orientation_;
    modal_.m.set_lock(lock);
  }
  modal_.from_gizmo = true;
  modal_apply();
  return true;
}

bool Editor2DScreen::modal_confirm() {
  if (!modal_active()) return false;
  const XformOp op = modal_.m.op();
  const std::size_t n = workspace_.xform_root_starts().size();
  std::string name;
  if (n == 1) {
    name = workspace_.xform_root_starts().front().name;
  }
  const bool changed = workspace_.end_xform();
  modal_.m.end();
  modal_keyboard_nav(false);
  if (!changed) return false;
  std::string label = xform_op_label(op);
  label += n == 1 ? " " + name : " " + std::to_string(n) + " entities";
  const bool pushed = workspace_.commit_step(label, std::move(modal_.before));
  mark_dirty_and_autosave();
  note(label);
  return pushed;
}

void Editor2DScreen::modal_cancel() {
  if (!modal_active()) return;
  const XformOp op = modal_.m.op();
  workspace_.cancel_xform();
  modal_.m.end();
  modal_keyboard_nav(false);
  note(std::string(xform_op_label(op)) + " cancelled");
}

void Editor2DScreen::modal_switch(XformOp op) {
  if (!modal_active() || op == XformOp::None) return;
  modal_.m.switch_op(op, modal_.mx, modal_.my);
  modal_.from_gizmo = false;
  modal_apply();
}

void Editor2DScreen::modal_mouse(float px, float py, bool ctrl, bool shift) {
  if (!modal_active()) return;
  modal_.mx = px;
  modal_.my = py;
  modal_.m.mouse(px, py, shift);
  modal_.snap = ModalTransform::snap_now(workspace_.snap_enabled(), ctrl);
  modal_apply();
}

void Editor2DScreen::modal_axis(int axis, bool shift) {
  if (!modal_active()) return;
  modal_.m.press_axis(axis, shift);
  modal_apply();
}

bool Editor2DScreen::modal_type(const std::string& chars) {
  if (!modal_active()) return false;
  bool ok = true;
  for (char c : chars) ok &= modal_.m.type_char(c);
  modal_apply();
  return ok;
}

void Editor2DScreen::modal_backspace() {
  if (!modal_active()) return;
  modal_.m.backspace();
  modal_apply();
}

void Editor2DScreen::modal_next_component() {
  if (!modal_active()) return;
  modal_.m.next_component();
  modal_apply();
}

void Editor2DScreen::modal_apply() {
  if (!modal_active()) return;
  const auto& roots = workspace_.xform_root_starts();
  if (roots.empty()) return;
  const Entity2D& A = roots[std::min(modal_.active, roots.size() - 1)];
  const ModalTransform& m = modal_.m;
  const AxisLock& lock = m.lock();
  const bool snap = modal_.snap;
  const float grid = std::max(workspace_.grid_size(), 1.0f);
  Workspace2D::Xform2D x;
  x.pivot_x = modal_.pivot_x;
  x.pivot_y = modal_.pivot_y;
  x.individual = pivot_mode_ == PivotMode::Individual;
  float spx, spy;
  world_to_canvas(modal_.pivot_x, modal_.pivot_y, &spx, &spy);

  switch (m.op()) {
    case XformOp::Move: {
      x.op = Op::Move;
      const int a = lock.axis >= 0 && lock.axis < 2 ? lock.axis : -1;
      float dirx = 1, diry = 0;
      if (a >= 0) Gizmo2D::axis_dir(a, lock.space == XformSpace::Local ? A.rotation : 0.0f, &dirx, &diry);
      const bool global_axis = a >= 0 && lock.space == XformSpace::Global;
      if (m.numeric()) {
        if (a >= 0) {
          const float amt = m.value(0, 0.0f);
          x.dx = dirx * amt;
          x.dy = diry * amt;
          modal_.shown[0] = amt;
        } else {
          x.dx = m.value(0, 0.0f);
          x.dy = m.value(1, 0.0f);
        }
      } else {
        float w0x, w0y, w1x, w1y;
        canvas_to_world(m.start_x(), m.start_y(), &w0x, &w0y);
        canvas_to_world(m.eff_x(), m.eff_y(), &w1x, &w1y);
        float dx = w1x - w0x, dy = w1y - w0y;
        if (m.negate_mouse()) {
          dx = -dx;
          dy = -dy;
        }
        if (a >= 0) {
          float amt = dx * dirx + dy * diry;
          if (snap) {
            if (global_axis) {
              const float base = a == 0 ? A.x : A.y;
              amt = workspace_.snap_always(base + amt) - base;
            } else {
              amt = std::round(amt / grid) * grid;
            }
          }
          x.dx = dirx * amt;
          x.dy = diry * amt;
          modal_.shown[0] = amt;
        } else {
          if (snap) {
            dx = workspace_.snap_always(A.x + dx) - A.x;
            dy = workspace_.snap_always(A.y + dy) - A.y;
          }
          x.dx = dx;
          x.dy = dy;
        }
      }
      if (a < 0) {
        modal_.shown[0] = x.dx;
        modal_.shown[1] = x.dy;
      }
      break;
    }
    case XformOp::Rotate: {
      x.op = Op::Rotate;
      float deg;
      if (m.numeric()) {
        deg = m.value(0, 0.0f);
      } else {
        deg = modal_.m.sweep(spx, spy) / kRad;
        if (m.negate_mouse()) deg = -deg;
        if (snap) deg = std::round(deg / kRotateSnapDeg) * kRotateSnapDeg;
      }
      x.degrees = deg;
      modal_.shown[0] = deg;
      break;
    }
    case XformOp::Scale: {
      x.op = Op::Scale;
      x.frame_deg = A.rotation;
      const int a = lock.axis >= 0 && lock.axis < 2 ? lock.axis : -1;
      float s0 = 1.0f, s1 = 1.0f;
      if (m.numeric()) {
        if (a >= 0) {
          (a == 0 ? s0 : s1) = m.value(0, 1.0f);
        } else {
          s0 = m.value(0, 1.0f);
          s1 = m.has_digits(1) ? m.value(1, s0) : s0;
        }
      } else {
        const float d0 = std::hypot(m.start_x() - spx, m.start_y() - spy);
        const float d1 = std::hypot(m.eff_x() - spx, m.eff_y() - spy);
        float r = d1 / std::max(d0, 1.0f);
        if (m.negate_mouse()) r = -r;
        if (a >= 0) {
          if (snap && !A.tilemap) {
            const float size0 = std::max(a == 0 ? A.w : A.h, 1.0f);
            r = std::max(grid, workspace_.snap_always(size0 * r)) / size0;
          }
          (a == 0 ? s0 : s1) = r;
        } else {
          if (snap) r = std::round(r / kScaleSnapStep) * kScaleSnapStep;
          s0 = s1 = r;
        }
      }
      auto clamp = [](float v) { return std::fabs(v) < 0.01f ? (v < 0 ? -0.01f : 0.01f) : v; };
      x.sx = clamp(s0);
      x.sy = clamp(s1);
      modal_.shown[0] = x.sx;
      modal_.shown[1] = x.sy;
      break;
    }
    case XformOp::None:
      return;
  }
  workspace_.apply_xform(x);
}

std::string Editor2DScreen::modal_header() const {
  if (!modal_active()) return {};
  const ModalTransform& m = modal_.m;
  const AxisLock& lock = m.lock();
  static const char* kAxis[2] = {"X", "Y"};
  auto comp = [&](int c, float v, const char* f) {
    const std::string typed = m.typed_text(c);
    if (m.component() == c && (m.numeric() || !typed.empty())) return "[" + typed + "|]";
    if (!typed.empty()) return typed;
    return fmt(v, f);
  };
  std::string h = xform_op_label(m.op());
  if (m.op() == XformOp::Rotate) {
    h += "  " + comp(0, modal_.shown[0], "%.2f") + " deg";
  } else {
    const char* f = m.op() == XformOp::Move ? "%.1f" : "%.3f";
    const int a = lock.axis >= 0 && lock.axis < 2 ? lock.axis : -1;
    if (a >= 0) {
      h += std::string("  ") + kAxis[a] + " " + comp(0, modal_.shown[0], f);
    } else if (m.op() == XformOp::Scale && !m.has_digits(1) && m.component() == 0) {
      h += "  " + comp(0, modal_.shown[0], f) + " (uniform)";
    } else {
      for (int k = 0; k < 2; ++k) h += std::string("  ") + kAxis[k] + " " + comp(k, modal_.shown[k], f);
    }
    h += "   " + m.constraint_text();
  }
  if (modal_.snap) h += "   snap";
  h += "   |  X/Y axis, type numbers, Ctrl snap, Shift precision, Enter/LMB confirm, Esc/RMB cancel";
  return h;
}

// ---- Alt resets + keys ---------------------------------------------------------------------

bool Editor2DScreen::reset_transform(XformOp what) {
  if (is_playing() || what == XformOp::None || modal_active()) return false;
  flush_pending_edit();
  const Op op = what == XformOp::Move ? Op::Move : (what == XformOp::Rotate ? Op::Rotate : Op::Scale);
  const bool ok = workspace_.clear_selection_transform(op);
  if (ok) {
    mark_dirty_and_autosave();
    note(what == XformOp::Move ? "Location cleared"
                               : (what == XformOp::Rotate ? "Rotation cleared" : "Size reset to 64 x 64"));
  }
  return ok;
}

void Editor2DScreen::handle_modal_keys() {
  const ModalKeys k = poll_modal_keys(modal_.m);
  if (k.cancel) {
    modal_cancel();
    return;
  }
  if (k.confirm) {
    modal_confirm();
    return;
  }
  if (k.switch_to != XformOp::None) modal_switch(k.switch_to);
  modal_apply();
}

// ---- Drawing ------------------------------------------------------------------------------

void Editor2DScreen::draw_gizmo_overlay(ImDrawList* dl, float x0, float y0) {
  // 2D cursor: red / white ring with cross hairs, as in Blender.
  {
    float cx, cy;
    world_to_canvas(cursor2d_x_, cursor2d_y_, &cx, &cy);
    const ImVec2 c(x0 + cx, y0 + cy);
    for (int i = 0; i < 16; ++i) {
      const float a0 = 6.2831853f * i / 16.0f, a1 = 6.2831853f * (i + 1) / 16.0f;
      dl->PathArcTo(c, 9.0f, a0, a1, 4);
      dl->PathStroke(i % 2 ? IM_COL32(240, 240, 240, 230) : IM_COL32(230, 40, 40, 230), ImDrawFlags_None, 2.0f);
    }
    const ImU32 k = IM_COL32(20, 20, 20, 200);
    dl->AddLine(ImVec2(c.x - 16, c.y), ImVec2(c.x - 5, c.y), k, 1.0f);
    dl->AddLine(ImVec2(c.x + 5, c.y), ImVec2(c.x + 16, c.y), k, 1.0f);
    dl->AddLine(ImVec2(c.x, c.y - 16), ImVec2(c.x, c.y - 5), k, 1.0f);
    dl->AddLine(ImVec2(c.x, c.y + 5), ImVec2(c.x, c.y + 16), k, 1.0f);
  }
  if (modal_active()) {
    float px, py;
    world_to_canvas(modal_.pivot_x, modal_.pivot_y, &px, &py);
    const AxisLock& lock = modal_.m.lock();
    const auto& roots = workspace_.xform_root_starts();
    if (lock.axis >= 0 && lock.axis < 2 && modal_.m.op() != XformOp::Rotate && !roots.empty()) {
      const Entity2D& A = roots[std::min(modal_.active, roots.size() - 1)];
      float dx, dy;
      Gizmo2D::axis_dir(lock.axis, lock.space == XformSpace::Local ? A.rotation : 0.0f, &dx, &dy);
      const float L = 4000.0f;
      dl->AddLine(ImVec2(x0 + px - dx * L, y0 + py - dy * L), ImVec2(x0 + px + dx * L, y0 + py + dy * L),
                  gizmo_axis_colour(lock.axis, 200), 1.5f);
    }
    if (modal_.m.op() != XformOp::Move) {
      const ImVec2 a(x0 + px, y0 + py), b(x0 + modal_.mx, y0 + modal_.my);
      const float dx = b.x - a.x, dy = b.y - a.y, d = std::sqrt(dx * dx + dy * dy);
      for (float t = 0; t < d; t += 10.0f) {
        const float t2 = std::min(d, t + 5.0f);
        dl->AddLine(ImVec2(a.x + dx * t / d, a.y + dy * t / d), ImVec2(a.x + dx * t2 / d, a.y + dy * t2 / d),
                    IM_COL32(240, 230, 200, 200), 1.5f);
      }
    }
    dl->AddCircleFilled(ImVec2(x0 + px, y0 + py), 3.5f, IM_COL32(240, 230, 200, 255));
    return;
  }
  const Entity2D* prim = workspace_.selected();
  if (!prim || gizmo_.mode == GizmoMode::None || is_playing() || tool_ != TileTool::Select) return;
  float cx, cy, ad, sd;
  gizmo_frame(&cx, &cy, &ad, &sd);
  std::vector<GizmoShape> shapes;
  gizmo_.layout(cx, cy, ad, sd, gizmo_hover_, &shapes);
  std::reverse(shapes.begin(), shapes.end());
  draw_gizmo_shapes(dl, shapes, x0, y0);
}

void Editor2DScreen::draw_transform_header() {
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 1.0f));
  const struct { GizmoMode m; const char* l; const char* tip; } modes[] = {
      {GizmoMode::None, "Tweak", "No gizmo: box handles, G / R / S transform (Q)"},
      {GizmoMode::Translate, "Move", "Move gizmo (W or 1)"},
      {GizmoMode::Rotate, "Rotate", "Rotate gizmo (2)"},
      {GizmoMode::Scale, "Scale", "Scale gizmo (T or 3)"},
      {GizmoMode::Transform, "Transform", "Move, rotate and scale gizmo (Y or 4)"}};
  for (const auto& m : modes) {
    const bool on = gizmo_.mode == m.m;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent());
    if (ImGui::SmallButton(m.l)) gizmo_.mode = m.m;
    if (on) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ux::SetTooltip("%s", m.tip);
    ImGui::SameLine(0, 3);
  }
  ImGui::SameLine(0, 10);
  ImGui::SetNextItemWidth(72);
  int sp = orientation_ == XformSpace::Local ? 1 : 0;
  if (ImGui::Combo("##orient2d", &sp, "Global\0Local\0")) {
    orientation_ = sp ? XformSpace::Local : XformSpace::Global;
  }
  if (ImGui::IsItemHovered()) ux::SetTooltip("Transform orientation (,)");
  ImGui::SameLine(0, 6);
  ImGui::SetNextItemWidth(150);
  if (open_pivot_menu_) {
    ImGui::OpenPopup("##PivotMenu2D");
    open_pivot_menu_ = false;
  }
  if (ImGui::BeginCombo("##pivot2d", pivot_label(pivot_mode_))) {
    for (int i = 0; i < kPivotModeCount; ++i) {
      const PivotMode p = static_cast<PivotMode>(i);
      if (ImGui::Selectable(pivot_label(p), p == pivot_mode_)) pivot_mode_ = p;
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) ux::SetTooltip("Pivot point (.)");
  if (ImGui::BeginPopup("##PivotMenu2D")) {
    ImGui::TextDisabled("Pivot Point");
    for (int i = 0; i < kPivotModeCount; ++i) {
      const PivotMode p = static_cast<PivotMode>(i);
      if (ux::MenuItem(pivot_label(p), nullptr, p == pivot_mode_)) pivot_mode_ = p;
    }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
  ImGui::SameLine(0, 12);
  ImGui::PushStyleColor(ImGuiCol_Text, modal_active() ? theme::Accent() : theme::TextMuted());
  if (modal_active()) {
    ImGui::TextUnformatted(modal_header().c_str());
  } else {
    ImGui::TextUnformatted(
        "G/R/S transform | LMB drag move | Alt+drag copy | RMB menu | Shift+RMB cursor | "
        "MMB/RMB-drag pan | wheel zoom | F frame | K collision");
  }
  ImGui::PopStyleColor();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
