// Modal transforms (G / R / S), the Blender-style gizmo, pivots, the 3D
// cursor and Alt+G / R / S for the 3D editor. A gizmo drag is a modal
// transform with the handle's constraint, so both share snapping,
// precision, pivots, multi-select and one undo step per confirm.
// docs/transforms.md has the user-facing list.

#include "editor/editor3d/Editor3DScreen.h"
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

using render3d::Quat;
using render3d::Transform;
using render3d::Vec3;
using scene3d::Entity3D;

namespace {

float snap_to(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

// The two axes of the plane with normal `n`, in X-Y-Z order.
void plane_axes(int n, int* p, int* q) {
  *p = n == 0 ? 1 : 0;
  *q = n == 2 ? 1 : 2;
}

std::string fmt(float v, const char* f = "%.3f") {
  char b[48];
  std::snprintf(b, sizeof(b), f, v);
  return b;
}

}  // namespace

// ---- Pivot + gizmo frame ----------------------------------------------------------------

Vec3 Editor3DScreen::pivot_point() const {
  std::vector<std::uint64_t> roots;
  for (std::uint64_t id : selection_roots()) {
    const Entity3D* e = scene_.find(id);
    if (e && !effectively_locked(*e)) roots.push_back(id);
  }
  if (roots.empty()) roots = selection_roots();
  if (roots.empty()) return cursor3d_;
  switch (pivot_mode_) {
    case PivotMode::Cursor: return cursor3d_;
    case PivotMode::Active: {
      const Entity3D* p = scene_.find(primary());
      if (p) return p->transform.position;
      break;
    }
    case PivotMode::Bounds: {
      Vec3 lo = scene_.find(roots[0])->transform.position, hi = lo;
      for (std::uint64_t id : roots) {
        lo = render3d::vmin(lo, scene_.find(id)->transform.position);
        hi = render3d::vmax(hi, scene_.find(id)->transform.position);
      }
      return (lo + hi) * 0.5f;
    }
    default: break;
  }
  Vec3 sum{0, 0, 0};
  for (std::uint64_t id : roots) sum += scene_.find(id)->transform.position;
  return sum / static_cast<float>(roots.size());
}

GizmoFrame Editor3DScreen::gizmo_frame() const {
  GizmoFrame f;
  if (modal_active()) {
    f.origin = modal_.pivot;
  } else {
    f.origin = pivot_point();
  }
  if (const Entity3D* p = scene_.find(primary())) {
    f.scale_axes = render3d::normalize(p->transform.rotation);
    if (orientation_ == XformSpace::Local) f.axes = f.scale_axes;
  }
  return f;
}

GizmoHandle Editor3DScreen::gizmo_pick(float px, float py, float vw, float vh) const {
  if (playing() || modal_active()) return {};
  const Entity3D* e = scene_.find(primary());
  if (!e || effectively_locked(*e)) return {};
  return gizmo_.pick(editor_camera(), vw, vh, gizmo_frame(), px, py);
}

// ---- Begin / end -------------------------------------------------------------------------

bool Editor3DScreen::modal_begin(XformOp op, float px, float py, float vw, float vh) {
  if (playing() || modal_active() || op == XformOp::None) return false;
  ModalState s;
  for (std::uint64_t id : selection_roots()) {
    const Entity3D* e = scene_.find(id);
    if (!e || effectively_locked(*e)) continue;
    s.roots.push_back(id);
    s.start.push_back(e->transform);
    s.all.emplace_back(id, e->transform);
    for (std::uint64_t d : scene3d::descendants_of(scene_.entities, id)) {
      if (const Entity3D* c = scene_.find(d)) s.all.emplace_back(d, c->transform);
    }
  }
  if (s.roots.empty()) return false;
  // The active element: the primary, or the root it hangs under.
  s.active = s.roots.size() - 1;
  for (const Entity3D* p = scene_.find(primary()); p; p = p->parent ? scene_.find(p->parent) : nullptr) {
    const auto it = std::find(s.roots.begin(), s.roots.end(), p->id);
    if (it != s.roots.end()) {
      s.active = static_cast<std::size_t>(it - s.roots.begin());
      break;
    }
    if (p->parent == p->id) break;
  }
  s.before = scene_;
  s.pivot = pivot_point();
  s.vw = vw > 1 ? vw : 1;
  s.vh = vh > 1 ? vh : 1;
  s.mx = px;
  s.my = py;
  s.snap = scene_.snap;
  modal_ = std::move(s);
  // Scale always works along each entity's own axes.
  modal_.m.begin(op, 3, op == XformOp::Scale ? XformSpace::Local : orientation_, px, py);
  modal_keyboard_nav(true);
  return true;
}

bool Editor3DScreen::modal_begin_handle(const GizmoHandle& h, float px, float py, float vw, float vh) {
  XformOp op = XformOp::None;
  switch (h.part) {
    case GizmoPart::MoveAxis:
    case GizmoPart::MovePlane:
    case GizmoPart::MoveFree: op = XformOp::Move; break;
    case GizmoPart::RotateAxis:
    case GizmoPart::RotateView:
    case GizmoPart::Trackball: op = XformOp::Rotate; break;
    case GizmoPart::ScaleAxis:
    case GizmoPart::ScalePlane:
    case GizmoPart::ScaleUniform: op = XformOp::Scale; break;
    default: return false;
  }
  if (!modal_begin(op, px, py, vw, vh)) return false;
  AxisLock lock;
  lock.space = op == XformOp::Scale ? XformSpace::Local : orientation_;
  if (h.part == GizmoPart::MoveAxis || h.part == GizmoPart::RotateAxis || h.part == GizmoPart::ScaleAxis) {
    lock.axis = h.axis;
  } else if (h.part == GizmoPart::MovePlane || h.part == GizmoPart::ScalePlane) {
    lock.plane = h.axis;
  }
  modal_.m.set_lock(lock);
  if (h.part == GizmoPart::Trackball) modal_.m.toggle_trackball();
  modal_.from_gizmo = true;
  gizmo_.set_drag(h);
  return true;
}

bool Editor3DScreen::modal_confirm() {
  if (!modal_active()) return false;
  modal_.m.end();
  gizmo_.cancel();
  modal_keyboard_nav(false);
  bool pushed = false;
  if (modal_.before) {
    if (dup_drag_) {
      push_undo("Duplicate", *modal_.before);  // the copies stay even when unmoved
      pushed = true;
    } else if (!(scene_ == *modal_.before)) {
      const XformOp op = modal_.m.op();
      (void)op;
      std::string what = modal_.shown_label;
      push_undo(what, *modal_.before);
      pushed = true;
    }
  }
  dup_drag_ = false;
  modal_.before.reset();
  return pushed;
}

void Editor3DScreen::modal_cancel() {
  if (!modal_active()) return;
  if (modal_.before) scene_ = *modal_.before;  // Alt+drag copies go too
  modal_.m.end();
  modal_.before.reset();
  gizmo_.cancel();
  dup_drag_ = false;
  modal_keyboard_nav(false);
}

void Editor3DScreen::modal_switch(XformOp op) {
  if (!modal_active() || op == XformOp::None) return;
  // Back to the start, then the new operation from the current mouse.
  for (const auto& [id, t] : modal_.all) {
    if (Entity3D* e = scene_.find(id)) e->transform = t;
  }
  modal_.m.begin(op, 3, op == XformOp::Scale ? XformSpace::Local : orientation_, modal_.mx, modal_.my);
  modal_.from_gizmo = false;
  gizmo_.cancel();
  modal_apply();
}

// ---- Input --------------------------------------------------------------------------------

void Editor3DScreen::modal_mouse(float px, float py, bool ctrl, bool shift) {
  if (!modal_active()) return;
  modal_.m.mouse(px, py, shift);
  modal_.mx = px;
  modal_.my = py;
  modal_.snap = ModalTransform::snap_now(scene_.snap, ctrl);
  modal_apply();
}

void Editor3DScreen::modal_axis(int axis, bool shift) {
  if (!modal_active()) return;
  modal_.m.press_axis(axis, shift);
  modal_apply();
}

bool Editor3DScreen::modal_type(const std::string& chars) {
  if (!modal_active()) return false;
  bool ok = true;
  for (char c : chars) ok = modal_.m.type_char(c) && ok;
  modal_apply();
  return ok;
}

void Editor3DScreen::modal_backspace() {
  if (!modal_active()) return;
  modal_.m.backspace();
  modal_apply();
}

void Editor3DScreen::modal_next_component() {
  if (!modal_active()) return;
  modal_.m.next_component();
  modal_apply();
}

void Editor3DScreen::modal_trackball() {
  if (!modal_active() || modal_.m.op() != XformOp::Rotate) return;
  modal_.m.toggle_trackball();
  modal_apply();
}

// ---- The transform ------------------------------------------------------------------------

void Editor3DScreen::modal_apply() {
  if (!modal_active()) return;
  ModalState& s = modal_;
  const ModalTransform& m = s.m;
  // Back to the start, then apply the whole transform again.
  for (const auto& [id, t] : s.all) {
    if (Entity3D* e = scene_.find(id)) e->transform = t;
  }
  const render3d::Camera3D cam = editor_camera();
  const float vw = s.vw, vh = s.vh;
  const Vec3 f = cam.forward();
  const GizmoSnap st = snap_steps();
  const Transform& a0 = s.start[s.active];
  const AxisLock lock = m.lock();
  const bool local = lock.space == XformSpace::Local;
  auto axis_of = [&](int a, std::size_t i) {
    return local ? render3d::normalize(s.start[i].rotation.rotate(Gizmo3D::axis_dir(a)))
                 : Gizmo3D::axis_dir(a);
  };
  float psx = vw * 0.5f, psy = vh * 0.5f;
  cam.project(s.pivot, vw, vh, &psx, &psy);
  const float sx = m.start_x(), sy = m.start_y(), ex = m.eff_x(), ey = m.eff_y();
  const std::size_t n = s.roots.size();
  const bool individual = pivot_mode_ == PivotMode::Individual;

  auto set = [&](std::size_t i, const Transform& t) {
    Entity3D* e = scene_.find(s.roots[i]);
    if (!e) return;
    e->transform = t;
    carry_descendants(s.roots[i], s.start[i]);
  };

  std::string label;
  if (m.op() == XformOp::Move) {
    std::vector<Vec3> d(n, Vec3{0, 0, 0});
    Vec3 shown{0, 0, 0};
    if (m.numeric()) {
      if (lock.axis >= 0) {
        const float v = m.value(0, 0.0f);
        for (std::size_t i = 0; i < n; ++i) d[i] = axis_of(lock.axis, i) * v;
        shown[lock.axis] = v;
      } else if (lock.plane >= 0) {
        int p, q;
        plane_axes(lock.plane, &p, &q);
        const float v0 = m.value(0, 0.0f), v1 = m.value(1, 0.0f);
        for (std::size_t i = 0; i < n; ++i) d[i] = axis_of(p, i) * v0 + axis_of(q, i) * v1;
        shown[p] = v0;
        shown[q] = v1;
      } else {
        const Vec3 v{m.value(0, 0.0f), m.value(1, 0.0f), m.value(2, 0.0f)};
        for (std::size_t i = 0; i < n; ++i) d[i] = v;
        shown = v;
      }
    } else if (lock.axis >= 0) {
      const Vec3 dir = axis_of(lock.axis, s.active);
      const float len = Gizmo3D::handle_length(cam, vh, s.pivot);
      float ox = 0, oy = 0, tx = 0, ty = 0;
      float amount = 0.0f;
      if (cam.project(s.pivot, vw, vh, &ox, &oy) && cam.project(s.pivot + dir * len, vw, vh, &tx, &ty)) {
        const float ux = tx - ox, uy = ty - oy;
        const float pl = std::sqrt(ux * ux + uy * uy);
        if (pl >= 1.0f) amount = ((ex - sx) * ux + (ey - sy) * uy) / pl * (len / pl);
      }
      if (m.negate_mouse()) amount = -amount;
      if (s.snap) {
        amount = local ? snap_to(amount, st.translate)
                       : snap_to(a0.position[lock.axis] + amount, st.translate) - a0.position[lock.axis];
      }
      for (std::size_t i = 0; i < n; ++i) d[i] = axis_of(lock.axis, i) * amount;
      shown[lock.axis] = amount;
    } else {
      const Vec3 nrm = lock.plane >= 0 ? axis_of(lock.plane, s.active) : f;
      const render3d::Ray r0 = cam.screen_ray(sx, sy, vw, vh), r1 = cam.screen_ray(ex, ey, vw, vh);
      const float pd = render3d::dot(nrm, s.pivot);
      float t0 = 0, t1 = 0;
      Vec3 delta{0, 0, 0};
      if (render3d::ray_plane(r0, nrm, pd, &t0) && render3d::ray_plane(r1, nrm, pd, &t1)) {
        delta = (r1.origin + r1.dir * t1) - (r0.origin + r0.dir * t0);
        delta = delta - nrm * render3d::dot(delta, nrm);
      }
      if (m.negate_mouse()) delta = -delta;
      if (local) {
        float c[3];
        for (int k = 0; k < 3; ++k) {
          c[k] = k == lock.plane ? 0.0f : render3d::dot(delta, axis_of(k, s.active));
          if (s.snap) c[k] = snap_to(c[k], st.translate);
          shown[k] = c[k];
        }
        for (std::size_t i = 0; i < n; ++i) {
          d[i] = axis_of(0, i) * c[0] + axis_of(1, i) * c[1] + axis_of(2, i) * c[2];
        }
      } else {
        if (s.snap) {
          for (int k = 0; k < 3; ++k) {
            if (k == lock.plane || std::fabs(delta[k]) < 1e-6f) continue;
            delta[k] = snap_to(a0.position[k] + delta[k], st.translate) - a0.position[k];
          }
        }
        for (std::size_t i = 0; i < n; ++i) d[i] = delta;
        shown = delta;
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      Transform t = s.start[i];
      t.position += d[i];
      set(i, t);
    }
    // Snap to surface: the active root's bounds rest on what is under the mouse.
    if ((surface_snap_ || (snap_surface_ && s.snap)) && !m.numeric() && !lock.any()) {
      std::vector<std::uint64_t> skip;
      for (const auto& p : s.all) skip.push_back(p.first);
      Vec3 hit;
      if (Entity3D* a = scene_.find(s.roots[s.active]);
          a && surface_point(s.mx, s.my, vw, vh, skip, &hit)) {
        const render3d::Aabb b = world_bounds(*a);
        const Vec3 off = hit - Vec3{b.center().x, b.min.y, b.center().z};
        for (std::size_t i = 0; i < n; ++i) {
          Entity3D* e = scene_.find(s.roots[i]);
          if (!e) continue;
          const Transform prev = e->transform;
          e->transform.position += off;
          carry_descendants(s.roots[i], prev);
        }
        shown = shown + off;
      }
    }
    s.shown = shown;
    label = "Move";
  } else if (m.op() == XformOp::Rotate) {
    std::vector<Quat> q(n);
    if (m.trackball()) {
      const Vec3 right = render3d::normalize(render3d::cross(f, cam.up));
      const Vec3 up = render3d::cross(right, f);
      float ax = (ex - sx) * 0.5f, ay = (ey - sy) * 0.5f;  // degrees per pixel
      if (m.negate_mouse()) {
        ax = -ax;
        ay = -ay;
      }
      if (m.numeric()) {
        ax = m.value(0, 0.0f);
        ay = m.value(1, 0.0f);
      } else if (s.snap) {
        ax = snap_to(ax, st.rotate);
        ay = snap_to(ay, st.rotate);
      }
      const Quat r = Quat::axis_angle(up, render3d::radians(ax)) * Quat::axis_angle(right, render3d::radians(ay));
      std::fill(q.begin(), q.end(), r);
      s.shown = {ax, ay, 0};
      label = "Trackball";
    } else {
      const int a = lock.axis >= 0 ? lock.axis : lock.plane;
      const Vec3 act = a >= 0 ? axis_of(a, s.active) : -f;
      float deg = 0.0f;
      const float sweep = s.m.sweep(psx, psy);
      if (m.numeric()) {
        deg = m.value(0, 0.0f);
      } else {
        deg = render3d::degrees(render3d::dot(act, f) > 0.0f ? sweep : -sweep);
        if (m.negate_mouse()) deg = -deg;
        if (s.snap) deg = snap_to(deg, st.rotate);
      }
      for (std::size_t i = 0; i < n; ++i) {
        q[i] = Quat::axis_angle(a >= 0 ? axis_of(a, i) : -f, render3d::radians(deg));
      }
      s.shown = {deg, 0, 0};
      label = "Rotate";
    }
    for (std::size_t i = 0; i < n; ++i) {
      Transform t = s.start[i];
      t.rotation = render3d::normalize(q[i] * s.start[i].rotation);
      if (!individual) t.position = s.pivot + q[i].rotate(s.start[i].position - s.pivot);
      set(i, t);
    }
  } else if (m.op() == XformOp::Scale) {
    const float d0 = std::hypot(sx - psx, sy - psy), d1 = std::hypot(ex - psx, ey - psy);
    float r = d0 > 1.0f ? d1 / d0 : 1.0f;
    Vec3 fac{1, 1, 1};
    if (m.numeric()) {
      if (lock.axis >= 0) {
        fac[lock.axis] = m.value(0, 1.0f);
      } else if (lock.plane >= 0) {
        int p, qa;
        plane_axes(lock.plane, &p, &qa);
        fac[p] = m.value(0, 1.0f);
        fac[qa] = m.value(1, fac[p]);
      } else {
        const float v0 = m.value(0, 1.0f);
        fac = {v0, m.value(1, v0), m.value(2, v0)};
      }
    } else {
      for (int k = 0; k < 3; ++k) {
        if (!lock.uses(k)) continue;
        fac[k] = r;
        if (s.snap) {
          if (lock.any() && std::fabs(a0.scale[k]) > 1e-6f) {
            fac[k] = std::max(st.scale, snap_to(a0.scale[k] * r, st.scale)) / a0.scale[k];
          } else {
            fac[k] = std::max(st.scale, snap_to(r, st.scale));
          }
        }
      }
    }
    const Quat ra = render3d::normalize(a0.rotation);
    for (std::size_t i = 0; i < n; ++i) {
      Transform t = s.start[i];
      for (int k = 0; k < 3; ++k) {
        float v = s.start[i].scale[k] * fac[k];
        if (std::fabs(v) < 0.01f) v = v < 0.0f ? -0.01f : 0.01f;
        t.scale[k] = v;
      }
      if (!individual) {
        const Vec3 off = ra.conjugate().rotate(s.start[i].position - s.pivot);
        t.position = s.pivot + ra.rotate(render3d::mul(off, fac));
      }
      set(i, t);
    }
    s.shown = fac;
    label = "Scale";
  }
  if (const Entity3D* e = scene_.find(s.roots[s.active]); e && n == 1) {
    s.shown_label = label + " " + e->name;
  } else {
    s.shown_label = label + " " + std::to_string(n) + " entities";
  }
}

std::string Editor3DScreen::modal_header() const {
  if (!modal_active()) return {};
  const ModalTransform& m = modal_.m;
  const AxisLock& lock = m.lock();
  static const char* kAxis[3] = {"X", "Y", "Z"};
  auto comp = [&](int c, float v, const char* f) {
    const std::string typed = m.typed_text(c);
    if (m.component() == c && (m.numeric() || !typed.empty())) return "[" + typed + "|]";
    if (!typed.empty()) return typed;
    return fmt(v, f);
  };
  std::string h = xform_op_label(m.op());
  const Vec3& v = modal_.shown;
  if (m.op() == XformOp::Rotate && m.trackball()) {
    h = "Trackball  X " + comp(0, v.x, "%.1f") + " deg  Y " + comp(1, v.y, "%.1f") + " deg";
  } else if (m.op() == XformOp::Rotate) {
    h += "  " + comp(0, v.x, "%.2f") + " deg";
  } else if (m.op() == XformOp::Move || m.op() == XformOp::Scale) {
    const char* f = "%.3f";
    if (lock.axis >= 0) {
      h += std::string("  ") + kAxis[lock.axis] + " " + comp(0, v[lock.axis], f);
    } else if (lock.plane >= 0) {
      int p, q;
      plane_axes(lock.plane, &p, &q);
      h += std::string("  ") + kAxis[p] + " " + comp(0, v[p], f) + "  " + kAxis[q] + " " + comp(1, v[q], f);
    } else if (m.op() == XformOp::Scale && !m.has_digits(1) && !m.has_digits(2) && m.component() == 0) {
      h += "  " + comp(0, v.x, f) + " (uniform)";
    } else {
      for (int k = 0; k < 3; ++k) h += std::string("  ") + kAxis[k] + " " + comp(k, v[k], f);
    }
  }
  h += "   " + m.constraint_text();
  if (modal_.snap) h += "   snap";
  h += "   |  X/Y/Z axis, Shift+X/Y/Z plane, type numbers, Ctrl snap, Shift precision, "
       "Enter/LMB confirm, Esc/RMB cancel";
  return h;
}

// ---- Gizmo (legacy API) -------------------------------------------------------------------

bool Editor3DScreen::gizmo_begin(float px, float py, float vw, float vh) {
  const GizmoHandle h = gizmo_pick(px, py, vw, vh);
  if (!h.any()) return false;
  return modal_begin_handle(h, px, py, vw, vh);
}

bool Editor3DScreen::gizmo_begin_axis(int axis, float px, float py, float vw, float vh) {
  if (axis < 0 || axis > 2) return false;
  const GizmoPart part = gizmo_.mode == GizmoMode::Rotate
                             ? GizmoPart::RotateAxis
                             : (gizmo_.mode == GizmoMode::Scale ? GizmoPart::ScaleAxis : GizmoPart::MoveAxis);
  return modal_begin_handle({part, axis}, px, py, vw, vh);
}

void Editor3DScreen::gizmo_move(float px, float py, float vw, float vh) {
  if (!modal_active()) return;
  modal_.vw = vw > 1 ? vw : 1;
  modal_.vh = vh > 1 ? vh : 1;
  modal_mouse(px, py, false, false);
}

void Editor3DScreen::gizmo_end() { modal_confirm(); }

void Editor3DScreen::gizmo_cancel() { modal_cancel(); }

bool Editor3DScreen::gizmo_begin_duplicate(float px, float py, float vw, float vh) {
  const GizmoHandle h = gizmo_pick(px, py, vw, vh);
  if (!h.any()) return false;
  const scene3d::Scene3D before = scene_;
  // Keep the primary's copy last so it stays the active entity.
  const std::uint64_t prim = primary();
  std::vector<std::uint64_t> sel = scene_.selection;
  sel.erase(std::remove(sel.begin(), sel.end(), prim), sel.end());
  sel.push_back(prim);
  scene_.selection = sel;
  if (duplicate_roots(false) == 0 || !scene_.find(primary())) {
    scene_ = before;
    return false;
  }
  if (!modal_begin_handle(h, px, py, vw, vh)) {
    scene_ = before;
    return false;
  }
  modal_.before = before;
  dup_drag_ = true;
  return true;
}

// ---- Alt+G / Alt+R / Alt+S ---------------------------------------------------------------

bool Editor3DScreen::reset_transform(XformOp what) {
  if (playing() || modal_active() || what == XformOp::None) return false;
  const scene3d::Scene3D before = scene_;
  for (std::uint64_t id : selection_roots()) {
    Entity3D* e = scene_.find(id);
    if (!e || effectively_locked(*e)) continue;
    const Transform prev = e->transform;
    if (what == XformOp::Move) e->transform.position = {0, 0, 0};
    if (what == XformOp::Rotate) e->transform.rotation = {0, 0, 0, 1};
    if (what == XformOp::Scale) e->transform.scale = {1, 1, 1};
    carry_descendants(id, prev);
  }
  if (scene_ == before) return false;
  push_undo(what == XformOp::Move ? "Clear Location"
                                  : (what == XformOp::Rotate ? "Clear Rotation" : "Clear Scale"),
            before);
  return true;
}

// ---- UI -----------------------------------------------------------------------------------

void Editor3DScreen::handle_modal_keys() {
  const ModalKeys k = poll_modal_keys(modal_.m);
  if (k.cancel) {
    modal_cancel();
    return;
  }
  if (k.confirm) {
    modal_confirm();
    return;
  }
  if (k.trackball) modal_.m.toggle_trackball();
  if (k.switch_to != XformOp::None) modal_switch(k.switch_to);
  modal_apply();
}


void Editor3DScreen::draw_gizmo_overlay(float x0, float y0, float vw, float vh) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const render3d::Camera3D cam = editor_camera();
  draw_cursor3d(x0, y0, vw, vh);
  if (modal_active()) {
    // Constraint lines through the pivot, plus a dashed line to the mouse.
    const AxisLock& lock = modal_.m.lock();
    float px = 0, py = 0;
    if (!cam.project(modal_.pivot, vw, vh, &px, &py)) return;
    const float len = Gizmo3D::handle_length(cam, vh, modal_.pivot) * 40.0f;
    for (int a = 0; a < 3; ++a) {
      if (!(lock.axis == a || (lock.plane >= 0 && lock.plane != a))) continue;
      Vec3 dir = Gizmo3D::axis_dir(a);
      if (lock.space == XformSpace::Local) {
        dir = modal_.start[modal_.active].rotation.rotate(dir);
      }
      // Clip the far end against the camera: step in until it projects.
      for (int sgn = -1; sgn <= 1; sgn += 2) {
        float l = len, ax = 0, ay = 0;
        while (l > 0.01f && !cam.project(modal_.pivot + dir * (l * sgn), vw, vh, &ax, &ay)) l *= 0.5f;
        if (l > 0.01f) {
          dl->AddLine(ImVec2(x0 + px, y0 + py), ImVec2(x0 + ax, y0 + ay), gizmo_axis_colour(a, 200), 1.5f);
        }
      }
    }
    if (modal_.m.op() != XformOp::Move) {
      const ImVec2 a(x0 + px, y0 + py), b(x0 + modal_.mx, y0 + modal_.my);
      const float dx = b.x - a.x, dy = b.y - a.y, d = std::sqrt(dx * dx + dy * dy);
      for (float t = 0; t < d; t += 10.0f) {
        const float t2 = std::min(d, t + 5.0f);
        dl->AddLine(ImVec2(a.x + dx * t / d, a.y + dy * t / d), ImVec2(a.x + dx * t2 / d, a.y + dy * t2 / d),
                    IM_COL32(20, 20, 20, 200), 1.5f);
      }
    }
    dl->AddCircleFilled(ImVec2(x0 + px, y0 + py), 3.5f, IM_COL32(240, 230, 200, 255));
    return;
  }
  const Entity3D* e = find(primary());
  if (!e || gizmo_.mode == GizmoMode::None) return;
  std::vector<GizmoShape> shapes;
  gizmo_.layout(cam, vw, vh, gizmo_frame(), gizmo_hover_, &shapes);
  // Draw back to front: big rings first, small handles last.
  std::reverse(shapes.begin(), shapes.end());
  draw_gizmo_shapes(dl, shapes, x0, y0);
}

void Editor3DScreen::draw_cursor3d(float x0, float y0, float vw, float vh) {
  float cx = 0, cy = 0;
  if (!editor_camera().project(cursor3d_, vw, vh, &cx, &cy)) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c(x0 + cx, y0 + cy);
  // Red and white ring with cross hairs, as in Blender.
  for (int i = 0; i < 16; ++i) {
    const float a0 = 6.2831853f * i / 16.0f, a1 = 6.2831853f * (i + 1) / 16.0f;
    dl->PathArcTo(c, 10.0f, a0, a1, 4);
    dl->PathStroke(i % 2 ? IM_COL32(240, 240, 240, 230) : IM_COL32(230, 40, 40, 230), ImDrawFlags_None, 2.0f);
  }
  const ImU32 k = IM_COL32(20, 20, 20, 220);
  dl->AddLine(ImVec2(c.x - 18, c.y), ImVec2(c.x - 6, c.y), k, 1.0f);
  dl->AddLine(ImVec2(c.x + 6, c.y), ImVec2(c.x + 18, c.y), k, 1.0f);
  dl->AddLine(ImVec2(c.x, c.y - 18), ImVec2(c.x, c.y - 6), k, 1.0f);
  dl->AddLine(ImVec2(c.x, c.y + 6), ImVec2(c.x, c.y + 18), k, 1.0f);
}

void Editor3DScreen::draw_transform_header(float x0, float y0, float vw) {
  // Viewport header: gizmo, orientation, pivot (+ the modal line).
  ImGui::SetCursorScreenPos(ImVec2(x0 + 8.0f, y0 + 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(28, 22, 16, 220));
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 32, 24, 220));
  const struct { GizmoMode m; const char* l; const char* tip; } modes[] = {
      {GizmoMode::None, "Tweak", "No gizmo: click selects, G / R / S transform (Q)"},
      {GizmoMode::Translate, "Move", "Move gizmo (W or 1)"},
      {GizmoMode::Rotate, "Rotate", "Rotate gizmo (E or 2)"},
      {GizmoMode::Scale, "Scale", "Scale gizmo (T or 3)"},
      {GizmoMode::Transform, "Transform", "Move, rotate and scale gizmo (Y or 4)"}};
  for (const auto& m : modes) {
    const bool on = gizmo_.mode == m.m;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent());
    if (ImGui::SmallButton(m.l)) gizmo_.mode = m.m;
    if (on) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.tip);
    ImGui::SameLine(0, 3);
  }
  ImGui::SameLine(0, 12);
  ImGui::SetNextItemWidth(78);
  int sp = orientation_ == XformSpace::Local ? 1 : 0;
  if (ImGui::Combo("##orient3d", &sp, "Global\0Local\0")) {
    orientation_ = sp ? XformSpace::Local : XformSpace::Global;
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Transform orientation (,)");
  ImGui::SameLine(0, 6);
  ImGui::SetNextItemWidth(150);
  if (ImGui::BeginCombo("##pivot3d", pivot_label(pivot_mode_))) {
    for (int i = 0; i < kPivotModeCount; ++i) {
      const PivotMode p = static_cast<PivotMode>(i);
      if (ImGui::Selectable(pivot_label(p), p == pivot_mode_)) pivot_mode_ = p;
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pivot point (.)");
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar();
  if (modal_active()) {
    const std::string h = modal_header();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = y0 + 32.0f;
    dl->AddRectFilled(ImVec2(x0, y - 3), ImVec2(x0 + vw, y + ImGui::GetTextLineHeight() + 3),
                      IM_COL32(20, 16, 12, 215));
    dl->AddText(ImVec2(x0 + 10, y), theme::U32(theme::Accent()), h.c_str());
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
