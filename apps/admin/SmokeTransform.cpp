// --smoke-transform (see SmokeTransform.h). Drives the same public calls
// the viewport keys and mouse use, without a window.
#include "SmokeTransform.h"

#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;
using ts::tombstone::Entity2D;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] transform FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool near3(const r3::Vec3& a, const r3::Vec3& b, float eps = 1e-3f) {
  return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
bool on_step(float v, float step) {
  const float k = v / step;
  return std::fabs(k - std::round(k)) < 1e-3f;
}

ed::ProjectInfo fresh_project(const std::string& id, ed::ProjectKind kind) {
  const fs::path dir = fs::absolute(fs::path("TombStoneProjects") / id);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  ed::ProjectInfo info;
  info.id = id;
  info.name = id;
  info.kind = kind;
  info.path = dir.string();
  return info;
}

constexpr float kW = 800.0f, kH = 600.0f;

// ---- 3D ---------------------------------------------------------------------------------

int run_3d() {
  ed::Editor3DScreen e(fresh_project("_xform3d_smoke", ed::ProjectKind::ThreeD));
  e.on_enter();
  const s3::Entity3D* crate = e.scene().find_named("Crate");
  if (!crate) return fail("3D starter crate");
  const std::uint64_t id = crate->id;
  r3::Transform t0;
  t0.position = {1.0f, 0.5f, -1.0f};
  e.set_transform(id, t0, "Place");
  e.set_snapping(false);
  e.select(id);
  auto pos = [&] { return e.find(id)->transform.position; };
  auto px = [&](const r3::Vec3& p, float* x, float* y) {
    return e.editor_camera().project(p, kW, kH, x, y);
  };
  float cx = 0, cy = 0;
  if (!px(pos(), &cx, &cy)) return fail("3D crate on screen");

  // G X 2 Enter: exactly +2 on X, one undo step; undo restores.
  std::size_t depth = e.undo_depth();
  if (!e.modal_begin(ed::XformOp::Move, cx + 40, cy, kW, kH)) return fail("3D G begins");
  e.modal_axis(0);
  e.modal_type("2");
  if (e.modal_header().find("[2|]") == std::string::npos) return fail("3D header shows typed 2");
  e.modal_mouse(cx + 200, cy + 90);  // the mouse no longer matters
  if (!e.modal_confirm()) return fail("3D G X 2 confirms");
  if (!near3(pos(), {3.0f, 0.5f, -1.0f})) return fail("3D G X 2 lands at x = 3");
  if (e.undo_depth() != depth + 1) return fail("3D G = one undo step");
  e.undo();
  if (!near3(pos(), t0.position)) return fail("3D undo of G X 2");

  // Minus + Backspace: G Y -1.5 (typed "-15", Backspace, "5" -> "-1.5").
  e.modal_begin(ed::XformOp::Move, cx, cy, kW, kH);
  e.modal_axis(1);
  e.modal_type("-1.6");
  e.modal_backspace();
  e.modal_type("5");
  e.modal_confirm();
  if (!near3(pos(), {1.0f, -1.0f, -1.0f})) return fail("3D G Y -1.5 with Backspace");
  e.undo();

  // Free move with the mouse, then Esc: back where it started, no step.
  depth = e.undo_depth();
  e.modal_begin(ed::XformOp::Move, cx, cy, kW, kH);
  e.modal_mouse(cx + 120, cy - 60);
  if (near3(pos(), t0.position)) return fail("3D free move follows the mouse");
  e.modal_cancel();
  if (!near3(pos(), t0.position) || e.undo_depth() != depth) return fail("3D cancel restores");

  // Shift+Z (plane XY): mouse moves never change Z.
  e.modal_begin(ed::XformOp::Move, cx, cy, kW, kH);
  e.modal_axis(2, true);
  e.modal_mouse(cx + 80, cy + 50);
  if (!near(pos().z, t0.position.z) || near3(pos(), t0.position)) return fail("3D Shift+Z keeps Z");
  e.modal_cancel();

  // Axis lock by mouse: only X changes.
  e.modal_begin(ed::XformOp::Move, cx, cy, kW, kH);
  e.modal_axis(0);
  e.modal_mouse(cx + 90, cy + 70);
  if (!near(pos().y, t0.position.y) || !near(pos().z, t0.position.z) || near(pos().x, t0.position.x)) {
    return fail("3D G X with the mouse moves X only");
  }
  // X again = local X (crate is unturned, so the same line); third = free.
  e.modal_axis(0);
  if (e.modal().lock().axis != 0 || e.modal().lock().space != ed::XformSpace::Local) {
    return fail("3D X twice = local X");
  }
  e.modal_axis(0);
  if (e.modal().lock().any()) return fail("3D X three times = free");
  e.modal_cancel();

  // Snapping: grid on, X lands on the 0.5 step; Ctrl flips it off / on.
  const float step = e.snap_steps().translate;
  e.set_snapping(true);
  e.modal_begin(ed::XformOp::Move, cx, cy, kW, kH);
  e.modal_axis(0);
  e.modal_mouse(cx + 37, cy);
  if (!on_step(pos().x, step)) return fail("3D snap on: X on the grid step");
  e.modal_mouse(cx + 37, cy, /*ctrl=*/true);
  if (on_step(pos().x, step)) return fail("3D Ctrl turns snap off mid-drag");
  e.modal_cancel();
  e.set_snapping(false);
  e.modal_begin(ed::XformOp::Move, cx, cy, kW, kH);
  e.modal_axis(0);
  e.modal_mouse(cx + 37, cy, /*ctrl=*/true);
  if (!on_step(pos().x, step)) return fail("3D Ctrl snaps with grid snap off");
  // Shift = precision: a tenth of the motion.
  e.modal_mouse(cx + 37, cy);
  const float full = pos().x - t0.position.x;
  e.modal_cancel();
  e.modal_begin(ed::XformOp::Move, cx, cy, kW, kH);
  e.modal_axis(0);
  e.modal_mouse(cx + 37, cy, false, true);
  const float fine = pos().x - t0.position.x;
  e.modal_cancel();
  if (!(std::fabs(fine) < std::fabs(full) * 0.5f) || near(fine, 0.0f, 1e-6f)) {
    return fail("3D Shift = precision");
  }

  // R Z 90: X axis turns onto Y. S 0.5: half size. S X 2: X only.
  e.modal_begin(ed::XformOp::Rotate, cx + 50, cy, kW, kH);
  e.modal_axis(2);
  e.modal_type("90");
  e.modal_confirm();
  {
    const r3::Vec3 xr = e.find(id)->transform.rotation.rotate({1, 0, 0});
    if (!near3(xr, {0, 1, 0}) || !near3(pos(), t0.position)) return fail("3D R Z 90");
  }
  e.modal_begin(ed::XformOp::Scale, cx + 50, cy, kW, kH);
  e.modal_type("0.5");
  e.modal_confirm();
  if (!near3(e.find(id)->transform.scale, {0.5f, 0.5f, 0.5f})) return fail("3D S 0.5");
  e.modal_begin(ed::XformOp::Scale, cx + 50, cy, kW, kH);
  e.modal_axis(0);
  e.modal_type("2");
  e.modal_confirm();
  if (!near3(e.find(id)->transform.scale, {1.0f, 0.5f, 0.5f})) return fail("3D S X 2");
  // Switching op mid-transform (G then R) keeps one session.
  depth = e.undo_depth();
  e.modal_begin(ed::XformOp::Move, cx, cy, kW, kH);
  e.modal_switch(ed::XformOp::Rotate);
  if (e.modal().op() != ed::XformOp::Rotate) return fail("3D G -> R switch");
  e.modal_type("45");
  e.modal_confirm();
  if (e.undo_depth() != depth + 1) return fail("3D switched op = one step");

  // Alt+G / Alt+R / Alt+S.
  depth = e.undo_depth();
  if (!e.reset_transform(ed::XformOp::Move) || !near3(pos(), {0, 0, 0})) return fail("3D Alt+G");
  if (!e.reset_transform(ed::XformOp::Rotate) ||
      !near3(e.find(id)->transform.rotation.rotate({1, 0, 0}), {1, 0, 0})) {
    return fail("3D Alt+R");
  }
  if (!e.reset_transform(ed::XformOp::Scale) || !near3(e.find(id)->transform.scale, {1, 1, 1})) {
    return fail("3D Alt+S");
  }
  if (e.undo_depth() != depth + 3) return fail("3D Alt resets: one step each");

  // Pivots with two cubes at x = -2 and x = +2 (+ a child on the right one).
  const std::uint64_t a = e.create_primitive("cube", {-2.0f, 0.0f, 4.0f});
  const std::uint64_t b = e.create_primitive("cube", {2.0f, 0.0f, 4.0f});
  const std::uint64_t kid = e.create_primitive("sphere", {2.0f, 1.0f, 4.0f});
  e.set_parent(kid, b);
  auto P = [&](std::uint64_t i) { return e.find(i)->transform.position; };
  e.select(a);
  e.select(b, true);
  e.set_snapping(false);
  float bx = 0, by = 0;
  px({0, 0, 4}, &bx, &by);
  const float ay = P(a).y;  // primitives rest on the ground
  const r3::Vec3 koff = P(kid) - P(b);
  struct Case { ed::PivotMode mode; r3::Vec3 a, b; };
  // R Y 180 about each pivot.
  const Case cases[] = {
      {ed::PivotMode::Median, {2, 0, 4}, {-2, 0, 4}},
      {ed::PivotMode::Bounds, {2, 0, 4}, {-2, 0, 4}},
      {ed::PivotMode::Individual, {-2, 0, 4}, {2, 0, 4}},
      {ed::PivotMode::Active, {6, 0, 4}, {2, 0, 4}},  // b is the primary
  };
  for (const Case& c : cases) {
    e.set_pivot_mode(c.mode);
    depth = e.undo_depth();
    e.modal_begin(ed::XformOp::Rotate, bx + 60, by, kW, kH);
    e.modal_axis(1);
    e.modal_type("180");
    e.modal_confirm();
    if (!near3(P(a), c.a + r3::Vec3{0, ay, 0}, 1e-2f) || !near3(P(b), c.b + r3::Vec3{0, ay, 0}, 1e-2f)) {
      std::cerr << "a " << P(a).x << ',' << P(a).y << ',' << P(a).z << " b " << P(b).x << ',' << P(b).y << ','
                << P(b).z << " primary " << e.primary() << " (a " << a << ", b " << b << ")\n";
      return fail(std::string("3D pivot ") + ed::pivot_label(c.mode));
    }
    if (e.undo_depth() != depth + 1) return fail("3D multi-select rotate = one step");
    // The child rides along with its parent.
    const r3::Vec3 kd = P(kid) - P(b);
    if (!near3(kd, {-koff.x, koff.y, -koff.z}, 1e-2f)) {
      return fail("3D child follows its parent");
    }
    e.undo();
    if (!near3(P(a), {-2, ay, 4}) || !near3(P(b), {2, ay, 4})) return fail("3D pivot undo");
  }
  // 3D cursor pivot: S 2 about (0, 0, 4) + (10, 0, 0) pushes both away.
  e.set_cursor3d({10.0f, ay, 4.0f});
  e.set_pivot_mode(ed::PivotMode::Cursor);
  e.modal_begin(ed::XformOp::Scale, bx + 60, by, kW, kH);
  e.modal_type("2");
  e.modal_confirm();
  if (!near3(P(a), {-14, ay, 4}, 1e-2f) || !near3(P(b), {-6, ay, 4}, 1e-2f)) return fail("3D cursor pivot");
  e.undo();
  e.set_pivot_mode(ed::PivotMode::Median);

  // Gizmo picking: centre circle, X arrow, uniform scale circle.
  e.select(id);
  e.gizmo().mode = ed::GizmoMode::Translate;
  const ed::GizmoFrame f = e.gizmo_frame();
  float ox = 0, oy = 0;
  px(f.origin, &ox, &oy);
  {
    const ed::GizmoHandle c = e.gizmo_pick(ox, oy, kW, kH);
    if (c.part != ed::GizmoPart::MoveFree) {
      std::cerr << "part " << int(c.part) << " axis " << c.axis << " at " << ox << ',' << oy << " sel " << e.primary()
                << " id " << id << '\n';
      return fail("3D pick centre = free move");
    }
  }
  const float len = ed::Gizmo3D::handle_length(e.editor_camera(), kH, f.origin);
  float hx = 0, hy = 0;
  px(f.origin + f.axes.rotate({1, 0, 0}) * (len * 0.7f), &hx, &hy);
  const ed::GizmoHandle hx_h = e.gizmo_pick(hx, hy, kW, kH);
  if (hx_h.part != ed::GizmoPart::MoveAxis || hx_h.axis != 0) return fail("3D pick X arrow");
  e.gizmo().mode = ed::GizmoMode::Scale;
  const ed::GizmoHandle u = e.gizmo_pick(ox + ed::Gizmo3D::kHandlePixels * 1.2f, oy, kW, kH);
  if (u.part != ed::GizmoPart::ScaleUniform) return fail("3D pick uniform scale circle");
  // A gizmo drag on the X arrow is a modal move locked to X.
  e.gizmo().mode = ed::GizmoMode::Translate;
  const r3::Vec3 before = pos();
  if (!e.modal_begin_handle(hx_h, hx, hy, kW, kH)) return fail("3D gizmo drag begins");
  e.modal_mouse(hx + 50, hy + 40);
  e.modal_confirm();
  if (!near(pos().y, before.y) || !near(pos().z, before.z) || near(pos().x, before.x)) {
    return fail("3D X arrow drag moves X only");
  }
  std::cout << "[smoke] transform 3D ok\n";
  return 0;
}

// ---- 2D ---------------------------------------------------------------------------------

std::uint64_t make_box(ed::Editor2DScreen& scr, const char* name, float x, float y, float w, float h) {
  const std::uint64_t id = scr.create_entity(name);
  if (Entity2D* e = scr.workspace().find(id)) {
    e->x = x;
    e->y = y;
    e->w = w;
    e->h = h;
    e->rotation = 0.0f;
  }
  return id;
}

int run_2d() {
  ed::Editor2DScreen scr(fresh_project("_xform2d_smoke", ed::ProjectKind::TwoD));
  scr.on_enter();
  ed::Workspace2D& w = scr.workspace();
  w.set_snap_enabled(false);
  const std::uint64_t id = make_box(scr, "Wagon", 100, 40, 100, 50);
  w.select(id);
  auto E = [&](std::uint64_t i) { return *w.find(i); };
  float cx = 0, cy = 0;
  scr.world_to_canvas(150, 65, &cx, &cy);

  // G X 20.
  std::size_t depth = w.undo_count();
  if (!scr.modal_begin(ed::XformOp::Move, cx, cy)) return fail("2D G begins");
  scr.modal_axis(0);
  scr.modal_type("20");
  scr.modal_mouse(cx + 300, cy + 300);
  scr.modal_confirm();
  if (!near(E(id).x, 120) || !near(E(id).y, 40)) return fail("2D G X 20");
  if (w.undo_count() != depth + 1) return fail("2D G = one undo step");
  scr.undo();
  if (!near(E(id).x, 100)) return fail("2D undo of G X 20");
  // G Tab: typed X then Y.
  scr.modal_begin(ed::XformOp::Move, cx, cy);
  scr.modal_type("-5");
  scr.modal_next_component();
  scr.modal_type("7");
  scr.modal_confirm();
  if (!near(E(id).x, 95) || !near(E(id).y, 47)) return fail("2D G -5 Tab 7");
  scr.undo();

  // Mouse, axis lock, cancel.
  scr.modal_begin(ed::XformOp::Move, cx, cy);
  scr.modal_axis(1);  // Y
  scr.modal_mouse(cx + 60, cy + 30);
  if (!near(E(id).x, 100) || near(E(id).y, 40)) return fail("2D G Y moves Y only");
  scr.modal_axis(0, true);  // Shift+X = Y in 2D
  if (scr.modal().lock().axis != 1) return fail("2D Shift+X = Y");
  depth = w.undo_count();
  scr.modal_cancel();
  if (!near(E(id).x, 100) || !near(E(id).y, 40) || w.undo_count() != depth) return fail("2D cancel restores");

  // Snap (grid), Ctrl flips it.
  w.set_snap_enabled(true);
  const float g = w.grid_size();
  scr.modal_begin(ed::XformOp::Move, cx, cy);
  scr.modal_mouse(cx + 13, cy + 29);
  if (!on_step(E(id).x, g) || !on_step(E(id).y, g)) return fail("2D snap on: corner on the grid");
  scr.modal_mouse(cx + 13, cy + 29, /*ctrl=*/true);
  if (on_step(E(id).x, g) && on_step(E(id).y, g)) return fail("2D Ctrl turns snap off");
  scr.modal_cancel();
  w.set_snap_enabled(false);

  // R 90 / S 0.5 / S X 2 about the box's own centre.
  scr.modal_begin(ed::XformOp::Rotate, cx + 50, cy);
  scr.modal_type("90");
  scr.modal_confirm();
  if (!near(E(id).rotation, 90) || !near(E(id).center_x(), 150) || !near(E(id).center_y(), 65)) {
    return fail("2D R 90");
  }
  scr.undo();
  // Rotate with snap by mouse: whole 15 degree steps.
  w.set_snap_enabled(true);
  scr.modal_begin(ed::XformOp::Rotate, cx + 50, cy);
  scr.modal_mouse(cx + 30, cy + 37);
  if (!on_step(E(id).rotation, 15.0f) || near(E(id).rotation, 0)) return fail("2D R snaps to 15 deg");
  scr.modal_cancel();
  w.set_snap_enabled(false);
  scr.modal_begin(ed::XformOp::Scale, cx + 50, cy);
  scr.modal_type("0.5");
  scr.modal_confirm();
  if (!near(E(id).w, 50) || !near(E(id).h, 25) || !near(E(id).center_x(), 150)) return fail("2D S 0.5");
  scr.undo();
  scr.modal_begin(ed::XformOp::Scale, cx + 50, cy);
  scr.modal_axis(0);
  scr.modal_type("2");
  scr.modal_confirm();
  if (!near(E(id).w, 200) || !near(E(id).h, 50)) return fail("2D S X 2");
  scr.undo();
  // Scale by mouse: twice the distance = twice the size.
  scr.modal_begin(ed::XformOp::Scale, cx + 40, cy);
  scr.modal_mouse(cx + 80, cy);
  if (!near(E(id).w, 200, 0.5f) || !near(E(id).h, 100, 0.5f)) return fail("2D S by mouse");
  scr.modal_cancel();

  // Alt+G / R / S.
  w.find(id)->rotation = 30.0f;
  depth = w.undo_count();
  if (!scr.reset_transform(ed::XformOp::Move) || !near(E(id).x, 0) || !near(E(id).y, 0)) return fail("2D Alt+G");
  if (!scr.reset_transform(ed::XformOp::Rotate) || !near(E(id).rotation, 0)) return fail("2D Alt+R");
  if (!scr.reset_transform(ed::XformOp::Scale) || !near(E(id).w, 64) || !near(E(id).h, 64) ||
      !near(E(id).center_x(), 50)) {
    return fail("2D Alt+S");
  }
  if (w.undo_count() != depth + 3) return fail("2D Alt resets: one step each");

  // Pivots: two boxes, centres (-100, 0) and (100, 0); a child under b;
  // a locked box that must stay put.
  const std::uint64_t a = make_box(scr, "A", -120, -20, 40, 40);
  const std::uint64_t b = make_box(scr, "B", 80, -20, 40, 40);
  const std::uint64_t kid = make_box(scr, "Kid", 80, 40, 40, 40);
  const std::uint64_t post = make_box(scr, "Post", 300, 300, 40, 40);
  w.set_parent(kid, b);
  w.find(post)->locked = true;
  w.select(a);
  w.add_to_selection(post);
  w.add_to_selection(b);  // primary
  scr.world_to_canvas(0, 0, &cx, &cy);
  struct Case { ed::PivotMode mode; float ax, bx; };
  const Case cases[] = {
      {ed::PivotMode::Median, 100, -100},
      {ed::PivotMode::Bounds, 200, 0},  // bounds include the kid (y) and the post
      {ed::PivotMode::Individual, -100, 100},
      {ed::PivotMode::Active, 300, 100},
  };
  for (const Case& c : cases) {
    scr.set_pivot_mode(c.mode);
    float pvx = 0, pvy = 0;
    scr.pivot_point(&pvx, &pvy);
    depth = w.undo_count();
    scr.modal_begin(ed::XformOp::Rotate, cx + 60, cy);
    scr.modal_type("180");
    scr.modal_confirm();
    const float ex_a = c.mode == ed::PivotMode::Individual ? -100 : 2 * pvx + 100;
    const float ex_b = c.mode == ed::PivotMode::Individual ? 100 : 2 * pvx - 100;
    if (!near(E(a).center_x(), ex_a, 0.05f) || !near(E(b).center_x(), ex_b, 0.05f)) {
      return fail(std::string("2D pivot ") + ed::pivot_label(c.mode));
    }
    if (c.mode == ed::PivotMode::Median && (!near(ex_a, c.ax) || !near(ex_b, c.bx))) {
      return fail("2D median pivot is the roots' centre");
    }
    if (c.mode == ed::PivotMode::Active && (!near(ex_a, c.ax) || !near(ex_b, c.bx))) {
      return fail("2D active pivot is the primary's centre");
    }
    if (w.undo_count() != depth + 1) return fail("2D multi-select rotate = one step");
    if (!near(E(post).x, 300) || !near(E(post).y, 300)) return fail("2D locked entity stays put");
    // Kid sat 60 below b; turned 180 it sits 60 above (b's own centre).
    if (!near(E(kid).center_x(), E(b).center_x(), 0.05f) ||
        !near(E(kid).center_y() - E(b).center_y(), c.mode == ed::PivotMode::Individual ? -60.0f : -60.0f, 0.05f)) {
      return fail("2D child turns with its parent");
    }
    scr.undo();
    if (!near(E(a).center_x(), -100) || !near(E(b).center_x(), 100)) return fail("2D pivot undo");
  }
  scr.set_cursor2d(300, 0);
  scr.set_pivot_mode(ed::PivotMode::Cursor);
  scr.modal_begin(ed::XformOp::Scale, cx + 60, cy);
  scr.modal_type("2");
  scr.modal_confirm();
  if (!near(E(a).center_x(), -500, 0.05f) || !near(E(b).center_x(), -100, 0.05f) || !near(E(b).w, 80)) {
    return fail("2D cursor pivot");
  }
  scr.undo();
  scr.set_pivot_mode(ed::PivotMode::Median);
  // G moves the child too.
  scr.modal_begin(ed::XformOp::Move, cx, cy);
  scr.modal_type("10");
  scr.modal_confirm();
  if (!near(E(kid).x, 90) || !near(E(a).x, -110)) return fail("2D G moves children along");
  scr.undo();

  // Gizmo: centre = free move, X arrow (right), Y arrow (down), scale circle.
  w.select(id);
  scr.set_gizmo_mode(ed::GizmoMode::Translate);
  float gx = 0, gy = 0;
  scr.world_to_canvas(E(id).center_x(), E(id).center_y(), &gx, &gy);
  if (scr.gizmo_pick(gx, gy).part != ed::GizmoPart::MoveFree) return fail("2D pick centre = free move");
  const float R = ed::Gizmo2D::kHandlePixels;
  const ed::GizmoHandle xh = scr.gizmo_pick(gx + R * 0.7f, gy);
  const ed::GizmoHandle yh = scr.gizmo_pick(gx, gy + R * 0.7f);
  if (xh.part != ed::GizmoPart::MoveAxis || xh.axis != 0) return fail("2D pick X arrow");
  if (yh.part != ed::GizmoPart::MoveAxis || yh.axis != 1) return fail("2D pick Y arrow");
  scr.set_gizmo_mode(ed::GizmoMode::Scale);
  if (scr.gizmo_pick(gx + R * 1.25f, gy).part != ed::GizmoPart::ScaleUniform) return fail("2D pick scale circle");
  scr.set_gizmo_mode(ed::GizmoMode::Rotate);
  if (scr.gizmo_pick(gx, gy - R).part != ed::GizmoPart::RotateView) return fail("2D pick rotate ring");
  scr.set_gizmo_mode(ed::GizmoMode::Translate);
  const Entity2D before = E(id);
  depth = w.undo_count();
  if (!scr.modal_begin_handle(xh, gx + R * 0.7f, gy)) return fail("2D gizmo drag begins");
  scr.modal_mouse(gx + R * 0.7f + 30, gy + 45);
  scr.modal_confirm();
  if (!near(E(id).y, before.y) || near(E(id).x, before.x) || w.undo_count() != depth + 1) {
    return fail("2D X arrow drag moves X only, one step");
  }
  // Local orientation: on a turned box the X arrow follows the turn.
  w.find(id)->rotation = 90.0f;
  scr.set_orientation(ed::XformSpace::Local);
  scr.world_to_canvas(E(id).center_x(), E(id).center_y(), &gx, &gy);
  const ed::GizmoHandle lx = scr.gizmo_pick(gx, gy + R * 0.7f);
  if (lx.part != ed::GizmoPart::MoveAxis || lx.axis != 0) return fail("2D local X arrow follows the turn");
  scr.set_orientation(ed::XformSpace::Global);
  scr.set_gizmo_mode(ed::GizmoMode::None);
  std::cout << "[smoke] transform 2D ok\n";
  return 0;
}

}  // namespace

int run_transform_smoke() {
  if (run_3d() != 0) return 1;
  if (run_2d() != 0) return 1;
  std::cout << "[smoke] transform pass ok\n";
  return 0;
}
