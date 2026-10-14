// Editor tools smoke: drives the same public paths the viewport, hierarchy,
// menus and hotkeys use, without a window.
#include "SmokeTools.h"

#include "editor/editor3d/Editor3DScreen.h"
#include "editor/screens/Editor2DScreen.h"
#include "render3d/Scene3D.h"
#include "runtime/World.h"
#include "scene/SceneJson.h"
#include "world3d/World3D.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;
namespace sj = ts::tombstone::scene_json;
namespace rt = ts::tombstone::runtime;
using ts::tombstone::Entity2D;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] editor tools pass FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) <= eps; }

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

std::uint64_t make_box(ed::Editor2DScreen& scr, const char* name, float x, float y, float w,
                       float h) {
  const std::uint64_t id = scr.create_entity(name);
  if (Entity2D* e = scr.workspace().find(id)) {
    e->x = x;
    e->y = y;
    e->w = w;
    e->h = h;
  }
  return id;
}

std::size_t index_of(const std::vector<Entity2D>& es, std::uint64_t id) {
  for (std::size_t i = 0; i < es.size(); ++i) {
    if (es[i].id == id) return i;
  }
  return es.size();
}

// ---- 2D ---------------------------------------------------------------------------

int check_2d() {
  const ed::ProjectInfo info = fresh_project("_tools2d_smoke", ed::ProjectKind::TwoD);
  ed::Editor2DScreen scr(info);
  scr.on_enter();
  ed::Workspace2D& w = scr.workspace();
  w.set_snap_enabled(false);
  w.set_object_snap(false);
  const std::uint64_t a = make_box(scr, "Alpha", 0, 0, 32, 32);
  const std::uint64_t b = make_box(scr, "Bravo", 100, 0, 32, 32);
  const std::uint64_t c = make_box(scr, "Charlie", 200, 50, 32, 32);
  // Park the seeded entities far away so they stay out of the snapping.
  for (const Entity2D& e : w.entities()) {
    if (e.id != a && e.id != b && e.id != c) {
      w.find(e.id)->x += 5000.0f;
      w.find(e.id)->y += 5000.0f;
    }
  }
  const std::size_t base_count = w.entities().size();

  // Resize from the bottom-right corner: top-left stays put. One step.
  w.select(a);
  std::size_t steps = w.undo_count();
  if (!scr.begin_handle_drag(1, 1, false, 32, 32)) return fail("2D grab resize handle");
  scr.update_handle_drag(64, 48, false);
  scr.end_handle_drag();
  const Entity2D* pa = w.find(a);
  if (pa->x != 0 || pa->y != 0 || pa->w != 64 || pa->h != 48) {
    return fail("2D corner resize (" + std::to_string(pa->w) + "x" + std::to_string(pa->h) + ")");
  }
  if (w.undo_count() != steps + 1) return fail("2D resize is one undo step");
  scr.undo();
  if (w.find(a)->w != 32 || w.find(a)->h != 32) return fail("2D resize undo");
  // Shift on an edge handle keeps the aspect (centred on the other axis).
  scr.begin_handle_drag(1, 0, false, 32, 16);
  scr.update_handle_drag(64, 16, true);
  scr.end_handle_drag();
  pa = w.find(a);
  if (pa->w != 64 || pa->h != 64 || pa->x != 0 || pa->y != -16) {
    return fail("2D Shift keeps aspect");
  }
  scr.undo();
  // Rotate knob: from straight above the centre to the right = +90 deg.
  if (!scr.begin_handle_drag(0, 0, true, 16, -24)) return fail("2D grab rotate knob");
  scr.update_handle_drag(56, 16, false);
  scr.end_handle_drag();
  if (!near(w.find(a)->rotation, 90.0f)) {
    return fail("2D rotate 90 (" + std::to_string(w.find(a)->rotation) + ")");
  }
  // A turned entity still picks inside its turned footprint.
  if (w.pick(16, 16) != a) return fail("2D pick turned entity");
  scr.undo();
  if (w.find(a)->rotation != 0.0f) return fail("2D rotate undo");

  // Alt+drag: copies on the spot, the drag moves the copies. One step.
  w.select(b);
  steps = w.undo_count();
  if (!scr.begin_duplicate_drag()) return fail("2D Alt+drag start");
  w.update_move(0, 40);
  scr.end_drag_move();
  if (w.entities().size() != base_count + 1 || w.find(b)->y != 0) {
    return fail("2D Alt+drag leaves the original");
  }
  const Entity2D* copy = w.selected();
  if (!copy || copy->id == b || copy->x != 100 || copy->y != 40) return fail("2D Alt+drag copy moved");
  if (w.undo_count() != steps + 1) return fail("2D Alt+drag is one step");
  scr.undo();
  if (w.entities().size() != base_count) return fail("2D Alt+drag undo");
  w.select(b);
  scr.begin_duplicate_drag();
  w.update_move(0, 40);
  scr.cancel_drag_move();
  if (w.entities().size() != base_count || w.undo_count() != steps) {
    return fail("2D Alt+drag Esc takes the copies back");
  }

  // Object snap: Charlie dragged near Bravo's left / top edges snaps on.
  w.set_object_snap(true);
  w.set_snap_threshold(6.0f);
  w.select(c);
  scr.begin_drag_move();
  w.update_move(-96.0f, -47.0f);  // to (104, 3)
  if (w.find(c)->x != 100.0f || w.find(c)->y != 0.0f || w.guides().empty()) {
    return fail("2D object snap to edges with guides (" + std::to_string(w.find(c)->x) + "," +
                std::to_string(w.find(c)->y) + ")");
  }
  scr.end_drag_move();
  if (!w.guides().empty()) return fail("2D guides cleared after the drag");
  scr.undo();
  w.set_object_snap(false);
  if (w.find(c)->x != 200.0f) return fail("2D snapped move undo");

  // Parenting: Charlie under Bravo follows moves, turns and deletes.
  if (!scr.reparent(c, b) || w.find(c)->parent != b) return fail("2D reparent");
  if (scr.reparent(b, c)) return fail("2D reparent refuses loops");
  w.select(b);
  scr.begin_drag_move();
  w.update_move(10.0f, 0.0f);
  scr.end_drag_move();
  if (w.find(b)->x != 110.0f || w.find(c)->x != 210.0f) return fail("2D child follows a move");
  scr.begin_handle_drag(0, 0, true, 126, -24);
  scr.update_handle_drag(126 + 40, 16, false);
  scr.end_handle_drag();
  // Charlie's centre (226, 66) orbits Bravo's centre (126, 16) by 90 deg.
  const Entity2D* pc = w.find(c);
  if (!near(pc->center_x(), 126.0f - 50.0f, 0.05f) || !near(pc->center_y(), 16.0f + 100.0f, 0.05f) ||
      !near(pc->rotation, 90.0f)) {
    return fail("2D child orbits a turn (" + std::to_string(pc->center_x()) + "," +
                std::to_string(pc->center_y()) + ")");
  }
  scr.undo();
  w.select(b);
  steps = w.undo_count();
  scr.delete_selected();
  if (w.find(b) || w.find(c)) return fail("2D delete takes children");
  scr.undo();
  if (!w.find(b) || !w.find(c) || w.find(c)->parent != b) return fail("2D delete undo keeps the link");

  // Hierarchy drops: before / inside, and unparent.
  if (!scr.hierarchy_drop(a, b, ed::Workspace2D::HierDrop::After) ||
      index_of(w.entities(), b) + 2 != index_of(w.entities(), a)) {  // after Bravo's subtree
    return fail("2D hierarchy drop after");
  }
  if (!scr.hierarchy_drop(a, b, ed::Workspace2D::HierDrop::Before) ||
      index_of(w.entities(), a) + 1 != index_of(w.entities(), b)) {
    return fail("2D hierarchy drop before");
  }
  if (!scr.hierarchy_drop(a, b, ed::Workspace2D::HierDrop::Inside) || w.find(a)->parent != b) {
    return fail("2D hierarchy drop inside");
  }
  scr.reparent(a, 0);
  scr.reparent(c, 0);

  // Group / ungroup.
  w.set_selection({a, c}, c);
  const std::uint64_t g = scr.group_selected();
  if (!g || w.find(a)->parent != g || w.find(c)->parent != g) return fail("2D group");
  const Entity2D* ge = w.find(g);
  const Entity2D* ca = w.find(a);
  const Entity2D* cc = w.find(c);
  if (ge->x != std::min(ca->x, cc->x) || ge->y != std::min(ca->y, cc->y) ||
      ge->x + ge->w != std::max(ca->x + ca->w, cc->x + cc->w) ||
      ge->y + ge->h != std::max(ca->y + ca->h, cc->y + cc->h)) {
    return fail("2D group bounds");
  }
  w.select(g);
  if (scr.ungroup_selected() != 2 || w.find(g) || w.find(a)->parent != 0) return fail("2D ungroup");

  // Lock / hide: no picks, no box selects; undoable.
  w.select(a);
  scr.lock_selected(true);
  if (w.pick(10, 10).has_value()) return fail("2D locked entity not pickable");
  scr.undo();
  if (w.pick(10, 10) != a) return fail("2D lock undo");
  w.select(b);
  scr.hide_selected(true);
  if (w.pick(110, 10).has_value() || !w.effectively_hidden(*w.find(b))) return fail("2D hide");
  scr.hide_selected(false);

  // Clipboard: Bravo + child across scenes.
  scr.reparent(c, b);
  w.select(b);
  if (!scr.copy_selected()) return fail("2D copy");
  const std::size_t before_paste = w.entities().size();
  if (scr.paste() != 1 || w.entities().size() != before_paste + 2) return fail("2D paste subtree");
  const std::uint64_t pasted = w.selection().front();
  if (w.children_of(pasted).size() != 1) return fail("2D paste keeps parent links");
  {
    const ed::ProjectInfo other = fresh_project("_tools2d_other", ed::ProjectKind::TwoD);
    ed::Editor2DScreen scr2(other);
    scr2.on_enter();
    const std::size_t n2 = scr2.workspace().entities().size();
    if (scr2.paste() != 1 || scr2.workspace().entities().size() != n2 + 2) {
      return fail("2D paste into another scene");
    }
    scr2.on_exit();
  }
  if (!ed::Editor2DScreen::clipboard_text().empty() && w.paste_json("not json", true).size() != 0) {
    return fail("2D bad clipboard pastes nothing");
  }
  scr.undo();
  scr.reparent(c, 0);

  // Align / distribute.
  w.set_selection({a, b, c}, a);
  scr.align_selected(ed::Workspace2D::Align::Top);
  if (w.find(a)->y != 0 || w.find(b)->y != 0 || w.find(c)->y != 0) return fail("2D align top");
  w.find(b)->x = 120;  // uneven gaps
  scr.distribute_selected(true);
  const float gap = (w.find(c)->x - (w.find(a)->x + w.find(a)->w) - w.find(b)->w) * 0.5f;
  if (!near(w.find(b)->x, w.find(a)->x + w.find(a)->w + gap)) return fail("2D distribute (" + std::to_string(w.find(b)->x) + ")");
  scr.align_selected(ed::Workspace2D::Align::CenterX);
  if (!near(w.find(a)->center_x(), w.find(c)->center_x())) return fail("2D align centre");
  scr.undo();
  scr.undo();
  scr.undo();

  // Frame, filter, add component.
  w.select(c);
  scr.frame_selected();
  if (!near(w.pan_x(), w.find(c)->center_x()) || !near(w.pan_y(), w.find(c)->center_y())) {
    return fail("2D frame selection");
  }
  scr.set_hierarchy_filter("char");
  if (scr.hierarchy_row_visible(*w.find(a)) || !scr.hierarchy_row_visible(*w.find(c))) {
    return fail("2D hierarchy filter");
  }
  scr.set_hierarchy_filter("");
  if (!scr.add_component(a, "Collider") || !w.find(a)->collider) return fail("2D add component");
  scr.undo();
  if (w.find(a)->collider) return fail("2D add component undo");

  // Undo everything we did, then redo it all: history holds.
  while (scr.undo()) {
  }
  while (scr.redo()) {
  }
  scr.on_exit();
  std::cout << "[smoke] 2D tools OK (resize + Shift aspect, rotate knob, turned pick, Alt+drag "
               "copy + Esc, object snap guides, parenting follows move/turn/delete, hierarchy "
               "drop, group/ungroup, lock/hide, copy/paste across scenes, align/distribute, "
               "frame, filter, add component, all undoable)\n";
  return 0;
}

int check_2d_format_runtime() {
  // v7 round trip with the new fields; v6 text loads with defaults; loops
  // are repaired.
  sj::SceneDoc doc;
  Entity2D p;
  p.id = 1;
  p.name = "Wagon";
  p.w = 64;
  p.h = 32;
  p.rotation = 30.0f;
  p.locked = true;
  Entity2D k;
  k.id = 2;
  k.name = "Wheel";
  k.parent = 1;
  k.hidden = true;
  doc.entities = {p, k};
  const std::string text = sj::write(doc);
  sj::SceneDoc back;
  std::string err;
  if (!sj::parse(text, &back, &err) || back.entities != doc.entities || sj::write(back) != text) {
    return fail("2D v7 round trip: " + err);
  }
  if (text.find("\"version\": " + std::to_string(sj::kSceneVersion)) == std::string::npos ||
      sj::kSceneVersion < 7) {
    return fail("2D writes v7");
  }
  // A v6 file: same entities without the new keys.
  sj::SceneDoc plain = doc;
  for (Entity2D& e : plain.entities) {
    e.rotation = 0.0f;
    e.parent = 0;
    e.locked = e.hidden = false;
  }
  std::string v6 = sj::write(plain);
  if (v6.find("\"rotation\"") != std::string::npos || v6.find("\"parent\"") != std::string::npos) {
    return fail("2D defaults are not written");
  }
  {
    const std::string cur = "\"version\": " + std::to_string(sj::kSceneVersion);
    v6.replace(v6.find(cur), cur.size(), "\"version\": 6");
  }
  sj::SceneDoc old;
  if (!sj::parse(v6, &old, &err) || old.entities.size() != 2 || old.entities[0].rotation != 0.0f ||
      old.entities[1].parent != 0 || old.entities[0].locked || old.entities[1].hidden) {
    return fail("2D v6 loads with defaults: " + err + "\n" + v6);
  }
  std::vector<Entity2D> loop = {p, k};
  loop[0].parent = 2;
  sj::repair_parents(loop);
  if (loop[0].parent != 0 && loop[1].parent != 0) return fail("2D parent loop repaired");

  // Runtime: a child rides with its parent; turned quads draw turned.
  ed::Workspace2D seed;
  seed.reset_defaults();
  std::vector<Entity2D> es = seed.entities();
  const Entity2D* player = nullptr;
  for (const Entity2D& e : es) {
    if (e.player) player = &e;
  }
  if (!player) return fail("2D seed player");
  const std::uint64_t player_id = player->id;
  Entity2D badge;
  badge.id = 900;
  badge.name = "Badge";
  badge.x = player->x;
  badge.y = player->y - 10;
  badge.w = 8;
  badge.h = 8;
  badge.parent = player_id;
  badge.rotation = 45.0f;
  es.push_back(badge);  // (player points into es: not used past here)
  player = nullptr;
  rt::World world;
  if (!world.build(es, "", &err)) return fail("2D runtime build: " + err);
  const float px0 = world.find(player_id)->data.x;
  rt::InputFrame in;
  in.slot(0).move_x = 1.0f;
  in.slot(0).connected = true;
  for (int i = 0; i < 30; ++i) world.step(in);
  const float moved = world.find(player_id)->data.x - px0;
  if (!(moved > 1.0f) || !near(world.find(900)->data.x - badge.x, moved, 0.01f)) {
    return fail("2D runtime child follows its parent");
  }
  std::vector<rt::DrawQuad> quads;
  world.build_draw_list({-10000, -10000, 10000, 10000}, 1.0f, {}, &quads);
  bool turned = false;
  for (const rt::DrawQuad& q : quads) turned |= q.entity == 900 && q.rotation == 45.0f;
  if (!turned) return fail("2D draw list carries rotation");
  std::cout << "[smoke] 2D scene v7 OK (rotation/parent/locked/hidden round trip, v6 loads, "
               "loop repair, runtime children follow, rotated draw quads)\n";
  return 0;
}

// ---- 3D ---------------------------------------------------------------------------

constexpr float kW = 800.0f, kH = 600.0f;

bool handle_point(ed::Editor3DScreen& e, int axis, float frac, float* x, float* y) {
  const s3::Entity3D* s = e.find(e.primary());
  const r3::Camera3D cam = e.editor_camera();
  const float len = ed::Gizmo3D::handle_length(cam, kH, s->transform.position);
  return cam.project(s->transform.position + ed::Gizmo3D::axis_dir(axis) * (len * frac), kW, kH,
                     x, y);
}

bool drag_x(ed::Editor3DScreen& e, bool duplicate) {
  e.gizmo().mode = ed::GizmoMode::Translate;
  float hx, hy, fx, fy;
  if (!handle_point(e, 0, 0.7f, &hx, &hy) || !handle_point(e, 0, 1.0f, &fx, &fy)) return false;
  const bool ok = duplicate ? e.gizmo_begin_duplicate(hx, hy, kW, kH) : e.gizmo_begin(hx, hy, kW, kH);
  if (!ok) return false;
  e.gizmo_move(hx + (fx - hx) * 4.0f, hy + (fy - hy) * 4.0f, kW, kH);
  return true;
}

int check_3d() {
  const ed::ProjectInfo info = fresh_project("_tools3d_smoke", ed::ProjectKind::ThreeD);
  ed::Editor3DScreen e(info);
  e.on_enter();
  e.set_snapping(true);
  const std::uint64_t crate = e.scene().find_named("Crate")->id;
  const std::uint64_t kid = e.create_primitive("cube", {2.0f, 1.5f, -1.0f});
  if (!kid || !e.set_parent(kid, crate) || e.find(kid)->parent != crate) return fail("3D parent");
  if (e.set_parent(crate, kid)) return fail("3D parent refuses loops");
  const std::size_t base = e.scene().entities.size();

  // Gizmo drag: the child rides along.
  e.select(crate);
  const r3::Vec3 c0 = e.find(crate)->transform.position, k0 = e.find(kid)->transform.position;
  if (!drag_x(e, false)) return fail("3D gizmo drag");
  e.gizmo_end();
  const float dx = e.find(crate)->transform.position.x - c0.x;
  if (!(dx > 0.4f) || !near(e.find(kid)->transform.position.x - k0.x, dx, 1e-4f)) {
    return fail("3D child follows the gizmo");
  }
  e.undo();
  if (!near(e.find(kid)->transform.position.x, k0.x, 1e-5f)) return fail("3D follow undo");

  // Turning the parent orbits the child.
  r3::Transform t = e.find(crate)->transform;
  t.rotation = r3::Quat::from_euler_deg({0.0f, 90.0f, 0.0f});
  e.set_transform(crate, t, "Rotate");
  const r3::Vec3 kp = e.find(kid)->transform.position;
  if (!near(kp.x, 2.0f, 1e-3f) || !near(kp.y, 1.5f, 1e-3f) || !near(kp.z, -1.0f, 1e-3f)) {
    return fail("3D child above the pivot stays on the axis");
  }
  e.undo();

  // Alt+drag copy with the gizmo: copies (subtree too), one step; Esc undoes.
  std::size_t depth = e.undo_depth();
  if (!drag_x(e, true)) return fail("3D Alt+drag start");
  e.gizmo_end();
  if (e.scene().entities.size() != base + 2 || e.undo_depth() != depth + 1 ||
      e.find(crate)->transform.position.x != c0.x) {
    return fail("3D Alt+drag copies the subtree");
  }
  const std::uint64_t crate_copy = e.primary();
  if (crate_copy == crate || e.children_of(crate_copy).size() != 1) return fail("3D copy keeps its child");
  e.undo();
  if (e.scene().entities.size() != base) return fail("3D Alt+drag undo");
  e.select(crate);
  drag_x(e, true);
  e.gizmo_cancel();
  if (e.scene().entities.size() != base || e.undo_depth() != depth) return fail("3D Alt+drag Esc");

  // Copy / paste across scenes.
  e.select(crate);
  if (!e.copy_selected()) return fail("3D copy");
  if (e.paste() != 1 || e.scene().entities.size() != base + 2) return fail("3D paste");
  {
    const ed::ProjectInfo other = fresh_project("_tools3d_other", ed::ProjectKind::ThreeD);
    ed::Editor3DScreen e2(other);
    e2.on_enter();
    const std::size_t n2 = e2.scene().entities.size();
    if (e2.paste() != 1 || e2.scene().entities.size() != n2 + 2 ||
        e2.children_of(e2.primary()).size() != 1) {
      return fail("3D paste into another scene");
    }
    e2.on_exit();
  }
  if (e.paste_json("{\"version\": 7, \"entities\": []}", true) != 0) return fail("3D 2D text refused");
  e.undo();

  // Group / ungroup, lock / hide (no picks), hierarchy drop.
  const std::uint64_t lamp = e.scene().find_named("Lamp Post") ? e.scene().find_named("Lamp Post")->id
                                                               : e.create_primitive("sphere", {-3, 0.5f, 0});
  e.select(crate);
  e.select(lamp, true);
  const std::uint64_t g = e.group_selected();
  if (!g || e.find(crate)->parent != g || e.find(lamp)->parent != g || e.find(kid)->parent != crate) {
    return fail("3D group keeps sub-links");
  }
  if (e.ungroup_selected() != 2 || e.find(g) || e.find(crate)->parent != 0) return fail("3D ungroup");
  float cx = 0, cy = 0;
  e.editor_camera().project(e.find(crate)->transform.position, kW, kH, &cx, &cy);
  if (e.pick(cx, cy, kW, kH) != crate) return fail("3D pick crate");
  e.select(crate);
  e.lock_selected(true);
  if (e.pick(cx, cy, kW, kH) == crate || drag_x(e, false)) return fail("3D locked: no pick, no gizmo");
  e.undo();
  e.hide_selected(true);
  if (e.pick(cx, cy, kW, kH) == crate) return fail("3D hidden: no pick");
  e.render_frame(160, 120);
  e.undo();
  if (!e.hierarchy_drop(kid, crate, 0) || e.find(kid)->parent != 0) return fail("3D drop before");
  if (!e.hierarchy_drop(kid, crate, 2) || e.find(kid)->parent != crate) return fail("3D drop inside");

  // Drop to ground: a box in the air lands on another box, then the ground.
  const std::uint64_t low = e.create_primitive("cube", {10.0f, 0.5f, 10.0f});
  const std::uint64_t high = e.create_primitive("cube", {10.0f, 6.0f, 10.0f});
  e.select(high);
  if (e.drop_to_ground() != 1 || !near(e.world_bounds(*e.find(high)).min.y, 1.0f, 1e-3f)) {
    return fail("3D drop onto the box below");
  }
  r3::Transform ht = e.find(high)->transform;
  ht.position.x = 20.0f;
  e.set_transform(high, ht);
  e.drop_to_ground();
  if (!near(e.world_bounds(*e.find(high)).min.y, 0.0f, 1e-3f)) return fail("3D drop to the ground");

  // Align / distribute.
  const std::uint64_t third = e.create_primitive("cube", {13.0f, 3.0f, 4.0f});
  e.select(low);
  e.select(high, true);
  e.select(third, true);
  e.align_selected(1, -1);
  const float y0 = e.world_bounds(*e.find(low)).min.y;
  if (!near(e.world_bounds(*e.find(high)).min.y, y0, 1e-3f) ||
      !near(e.world_bounds(*e.find(third)).min.y, y0, 1e-3f)) {
    return fail("3D align min Y");
  }
  e.distribute_selected(0);
  if (!near(e.find(third)->transform.position.x, 15.0f, 1e-3f)) return fail("3D distribute X");
  depth = e.undo_depth();
  e.undo();
  e.undo();
  if (e.undo_depth() != depth - 2) return fail("3D align/distribute undo");

  // Surface pick under the screen centre (ground or an entity), and Ctrl
  // surface snapping during a move drag puts the box on it.
  r3::Vec3 hit;
  if (!e.surface_point(kW * 0.5f, kH * 0.5f, kW, kH, {}, &hit)) return fail("3D surface point");

  // Delete takes children; undo restores the link.
  e.select(crate);
  e.delete_selected();
  if (e.find(crate) || e.find(kid)) return fail("3D delete takes children");
  e.undo();
  if (!e.find(kid) || e.find(kid)->parent != crate) return fail("3D delete undo");

  // Runtime: children follow the rider.
  s3::Scene3D sc = e.scene();
  const s3::Entity3D* rider = nullptr;
  for (const s3::Entity3D& en : sc.entities) {
    if (en.player) rider = &en;
  }
  if (!rider) return fail("3D starter has a rider");
  s3::Entity3D hat;
  hat.id = sc.next_id();
  hat.name = "Hat";
  hat.parent = rider->id;
  hat.transform.position = rider->transform.position + r3::Vec3{0.0f, 2.0f, 0.0f};
  const r3::Vec3 hat0 = hat.transform.position, rider0 = rider->transform.position;
  const std::uint64_t rider_id = rider->id;
  sc.entities.push_back(hat);
  ts::tombstone::world3d::World3D world;
  std::string err;
  if (!world.start(sc, info.path, &err)) return fail("3D world: " + err);
  ts::tombstone::world3d::Input3D in;
  in.move_forward = 1.0f;
  world.run_ticks(40, in);
  const s3::Entity3D* r1 = nullptr;
  const s3::Entity3D* h1 = nullptr;
  for (const s3::Entity3D& en : world.entities()) {
    if (en.id == rider_id) r1 = &en;
    if (en.id == hat.id) h1 = &en;
  }
  const r3::Vec3 dr = r1->transform.position - rider0, dh = h1->transform.position - hat0;
  if (r3::length(dr) < 0.2f || r3::length(dr - dh) > 1e-3f) return fail("3D runtime child follows");
  world.stop();
  e.on_exit();
  std::cout << "[smoke] 3D tools OK (parenting: gizmo/turn/delete/runtime follow, Alt+drag copy "
               "+ Esc, copy/paste across scenes, group/ungroup, lock/hide, hierarchy drop, drop "
               "to ground, align/distribute, surface pick, all undoable)\n";
  return 0;
}

int check_3d_format() {
  s3::Scene3D s = s3::make_starter_scene();
  s.entities[1].parent = s.entities[0].id;
  s.entities[1].locked = true;
  s.entities[0].editor_hidden = true;
  const std::string text = s3::write(s);
  s3::Scene3D back;
  std::string err;
  if (!s3::parse(text, &back, &err) || !(back == s) || s3::write(back) != text ||
      s3::kScene3DVersion < 9 ||
      text.find("\"version\": " + std::to_string(s3::kScene3DVersion)) == std::string::npos) {
    return fail("3D v9 round trip: " + err);
  }
  std::string v8 = s3::write(s3::make_starter_scene());
  {
    const std::string cur = "\"version\": " + std::to_string(s3::kScene3DVersion);
    v8.replace(v8.find(cur), cur.size(), "\"version\": 8");
  }
  s3::Scene3D old;
  if (!s3::parse(v8, &old, &err) || old.entities[1].parent != 0 || old.entities[1].locked) {
    return fail("3D v8 loads: " + err);
  }
  std::vector<s3::Entity3D> loop = s.entities;
  loop[0].parent = loop[1].id;  // 1 -> 0 -> 1
  s3::repair_parents(&loop);
  if (loop[0].parent != 0 && loop[1].parent != 0) return fail("3D loop repaired");
  std::cout << "[smoke] 3D scene v9 OK (parent/locked/editor_hidden round trip, v8 loads, loop "
               "repair)\n";
  return 0;
}

}  // namespace

int run_tools_smoke() {
  if (check_2d() != 0) return 1;
  if (check_2d_format_runtime() != 0) return 1;
  if (check_3d_format() != 0) return 1;
  if (check_3d() != 0) return 1;
  return 0;
}
