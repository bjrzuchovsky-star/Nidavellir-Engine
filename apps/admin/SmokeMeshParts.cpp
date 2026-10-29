#include "SmokeMeshParts.h"

#include "editor/assets/AssetDb.h"
#include "editor/assets/AssetImport.h"
#include "editor/assets/ModelSplit.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/ProjectInfo.h"
#include "physics3d/Physics3D.h"
#include "plugins/physics_box2d/Box2DWorld.h"
#include "plugins/physics_jolt/JoltWorld.h"
#include "render3d/ColliderFit.h"
#include "render3d/Gltf.h"
#include "render3d/HullFit.h"
#include "render3d/Mesh.h"
#include "render3d/MeshSplit.h"
#include "render3d/ModelImport.h"
#include "runtime/SpritePoly.h"
#include "scene/JsonTree.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
namespace ed = ts::tombstone::editor;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;
namespace p3 = ts::tombstone::physics3d;
namespace p2 = ts::tombstone::physics2d;

namespace {

int fail(const std::string& what) {
  std::cerr << "[smoke] mesh parts FAILED: " << what << '\n';
  return 1;
}

bool near(float a, float b, float eps = 0.05f) { return std::fabs(a - b) <= eps; }

r3::MeshData placed_box(r3::Vec3 center, r3::Vec3 size) {
  r3::MeshData m = r3::make_cube(1.0f);
  for (r3::Vertex3D& v : m.vertices) v.position = r3::mul(v.position, size) + center;
  return m;
}

void append_mesh(r3::MeshData* dst, const r3::MeshData& src) {
  const auto base = static_cast<std::uint32_t>(dst->vertices.size());
  dst->vertices.insert(dst->vertices.end(), src.vertices.begin(), src.vertices.end());
  for (std::uint32_t i : src.indices) dst->indices.push_back(base + i);
}

r3::Model model_of(r3::MeshData mesh) {
  r3::Model m;
  r3::ModelPrimitive p;
  p.mesh = std::move(mesh);
  p.node = "body";
  m.primitives.push_back(std::move(p));
  r3::update_bounds(&m);
  return m;
}

}  // namespace

int run_mesh_parts_smoke() {
  const fs::path dir = fs::temp_directory_path() / "tombstone_mesh_parts_smoke";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "assets" / "Models", ec);

  // --- Split the bundled lantern (post, arm, glass) -------------------------
  const fs::path lantern = dir / "assets" / "Models" / "lantern.gltf";
  std::string err;
  if (!r3::write_sample_lantern_gltf(lantern.string(), &err)) return fail("write lantern: " + err);
  r3::Model loaded;
  if (!r3::load_model(lantern.string(), &loaded, &err)) return fail("load lantern: " + err);
  if (r3::model_part_count(loaded) < 3) return fail("lantern should be three parts");

  ed::assetdb::AssetDb db;
  db.set_importer(ed::assetdb::import_asset);
  db.open(dir.string());
  db.scan();
  ed::ModelSplitOptions keep;
  keep.recentre = false;
  keep.fit_collider = true;
  ed::ModelSplitResult kept;
  if (!ed::split_model_asset(db, "assets/Models/lantern.gltf", keep, &kept)) return fail(kept.error);
  if (kept.rels.size() != 3) return fail("split count " + std::to_string(kept.rels.size()));
  bool saw_post = false, saw_glass = false;
  for (std::size_t i = 0; i < kept.names.size(); ++i) {
    if (kept.names[i] == "post") {
      saw_post = true;
      if (kept.origins[i].y < 0.5f) return fail("post origin should sit up the post");
    }
    if (kept.names[i] == "glass") saw_glass = true;
    const ed::assetdb::AssetDb::Entry* e = db.by_path(kept.rels[i]);
    if (!e) return fail("missing meta for " + kept.rels[i]);
    if (e->meta.settings.str("pivot") != "keep") return fail("pivot keep not stored");
    if (e->meta.settings.str("split_from") != "assets/Models/lantern.gltf") return fail("split_from");
    if (!e->meta.info.has("fitted")) return fail("part has no fitted cache");
    if (std::fabs(e->meta.settings.num("unit_scale", 1) - 1.0) > 1.0e-4) return fail("part kept a unit scale");
  }
  if (!saw_post || !saw_glass) return fail("part names post/glass missing");

  // Kept positions: the post's vertices are still up in the air.
  r3::Model post;
  std::string post_rel;
  for (std::size_t i = 0; i < kept.names.size(); ++i)
    if (kept.names[i] == "post") post_rel = kept.rels[i];
  if (!r3::load_model((dir / post_rel).string(), &post, &err)) return fail("reload post: " + err);
  if (post.bounds.center().y < 0.5f) return fail("keep-position post was recentred");

  ed::ModelSplitOptions mid;
  mid.recentre = true;
  ed::ModelSplitResult centred;
  if (!ed::split_model_asset(db, "assets/Models/lantern.gltf", mid, &centred)) return fail(centred.error);
  for (std::size_t i = 0; i < centred.names.size(); ++i) {
    if (centred.names[i].rfind("post", 0) != 0) continue;
    r3::Model again;
    if (!r3::load_model((dir / centred.rels[i]).string(), &again, &err)) return fail(err);
    if (std::fabs(again.bounds.center().y) > 0.05f) return fail("recentred post is not at the origin");
  }

  // Place as a parent with children, then undo and redo (files included).
  {
  ed::ProjectInfo info;
  info.id = "mesh_parts";
  info.name = "mesh parts";
  info.kind = ed::ProjectKind::ThreeD;
  info.path = dir.string();
  ed::Editor3DScreen screen(info);
  screen.on_enter();
  const std::size_t before = screen.scene().entities.size();
  const std::uint64_t root = screen.place_model_parts("assets/Models/lantern.gltf", {2.0f, 0.0f, -1.0f}, true, true);
  if (!root) return fail("place parts");
  int children = 0;
  bool child_collider = false;
  for (const s3::Entity3D& e : screen.scene().entities) {
    if (e.parent != root) continue;
    ++children;
    if (e.mesh && e.mesh->source.find("post") != std::string::npos) {
      if (std::fabs(e.transform.position.x - 2.0f) > 0.02f) return fail("child x");
      if (e.transform.position.y < 0.5f) return fail("recentred child was not put back up the post");
    }
    if (e.collider) child_collider = true;
  }
  if (children != 3) return fail("expected 3 children, got " + std::to_string(children));
  if (!child_collider) return fail("parts were placed without colliders");
  if (!screen.undo()) return fail("undo place");
  if (screen.scene().entities.size() != before) return fail("undo did not restore the scene");
  if (!screen.redo()) return fail("redo place");
  if (screen.scene().entities.size() != before + 4) return fail("redo did not bring the parts back");
  screen.undo();  // leave the scene as it was; the reused part files stay

  // A fresh split from the editor is one undo step, files included.
  std::size_t files_before = 0;
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
    if (it->is_regular_file(ec)) ++files_before;
  if (!screen.split_model_asset("assets/Models/lantern.gltf", false, true)) return fail("editor split");
  std::size_t files_mid = 0;
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
    if (it->is_regular_file(ec)) ++files_mid;
  if (files_mid <= files_before) return fail("editor split wrote nothing");
  if (!screen.undo()) return fail("undo split");
  std::size_t files_after = 0;
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
    if (it->is_regular_file(ec)) ++files_after;
  if (files_after != files_before) return fail("undo split left files behind");
  if (!screen.redo()) return fail("redo split");
  files_after = 0;
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec))
    if (it->is_regular_file(ec)) ++files_after;
  if (files_after != files_mid) return fail("redo split did not restore the files");
  }

  // --- Oriented box is tighter on a diamond than the axis-aligned box ------
  // A long bar turned 45 degrees. A cube's corners are isotropic, so the
  // covariance cannot tell its edges from the world axes; a bar can.
  r3::MeshData diamond = placed_box({}, {2.0f, 0.3f, 0.3f});
  const float cs = 0.70710678f;
  for (r3::Vertex3D& v : diamond.vertices) {
    const float x = v.position.x, z = v.position.z;
    v.position.x = x * cs - z * cs;
    v.position.z = x * cs + z * cs;
  }
  r3::Model dia = model_of(std::move(diamond));
  s3::Collider3D box, obb;
  if (!r3::fit_collider(dia, r3::ColliderFit::Box, &box)) return fail("box fit");
  if (!r3::fit_collider(dia, r3::ColliderFit::Obb, &obb)) return fail("obb fit");
  const float box_vol = box.size.x * box.size.y * box.size.z;
  const float obb_vol = obb.size.x * obb.size.y * obb.size.z;
  if (!(obb_vol < box_vol * 0.8f)) return fail("oriented box was not tighter than the axis-aligned one");
  if (obb.rotation_deg == r3::Vec3{}) return fail("oriented box has no rotation");
  // A plain cube stays a plain box.
  s3::Collider3D cube_fit;
  if (!r3::fit_collider(model_of(r3::make_cube(1.0f)), r3::ColliderFit::Obb, &cube_fit)) return fail("cube obb");
  if (!(cube_fit.rotation_deg == r3::Vec3{})) return fail("a cube was given a rotated box");

  // --- Convex hull vertex limit, and an L cut into pieces -------------------
  r3::MeshData sphere = r3::make_sphere(1.0f, 12, 8);
  r3::Model sm = model_of(std::move(sphere));
  r3::FitParams few;
  few.hull_vertices = 8;
  if (!r3::ensure_fitted(&sm, few)) return fail("sphere hull");
  if (sm.fitted.convex.empty() || sm.fitted.convex[0].size() > 8 || sm.fitted.convex[0].size() < 4) {
    return fail("vertex limit not applied");
  }
  r3::MeshData ell;
  append_mesh(&ell, placed_box({1.0f, 0.25f, 0.0f}, {2.0f, 0.5f, 3.0f}));
  append_mesh(&ell, placed_box({0.25f, 1.0f, 0.0f}, {0.5f, 2.0f, 3.0f}));
  r3::Model lm = model_of(std::move(ell));
  r3::FitParams dec;
  dec.decompose_max = 8;
  dec.hull_vertices = 16;
  if (!r3::ensure_fitted(&lm, dec)) return fail("L hull");
  if (lm.fitted.pieces.size() < 2) return fail("L was not cut into convex pieces");
  const r3::Vec3 on_base{1.5f, 0.25f, 0.0f};
  const r3::Vec3 in_notch{1.5f, 1.5f, 0.0f};
  bool base_in = false, notch_in = false;
  for (const auto& piece : lm.fitted.pieces) {
    base_in = base_in || r3::hull_contains(piece, on_base, 0.08f);
    notch_in = notch_in || r3::hull_contains(piece, in_notch, 0.02f);
  }
  if (!base_in) return fail("a convex piece missed the base of the L");
  if (notch_in) return fail("a convex piece filled the empty corner of the L");

  // Cache round trip, and a changed mesh no longer matches.
  ts::tombstone::json::Value info_v = ts::tombstone::json::Value::object();
  info_v.set("fitted", r3::fitted_to_json(lm.fitted));
  r3::Model cached = lm;
  cached.fitted = {};
  r3::apply_fitted_cache(&cached, info_v, dec);
  if (!cached.fitted.matches(r3::mesh_fingerprint(lm), dec.hull_vertices, dec.decompose_max)) {
    return fail("fitted cache did not load");
  }
  cached.primitives[0].mesh.vertices[0].position.x += 3.0f;
  if (r3::mesh_fingerprint(cached) == lm.fitted.fingerprint) return fail("fingerprint ignored an edit");

  // --- Jolt: a triangle-mesh floor stops a dynamic convex crate -------------
  {
    r3::Model floor = model_of(placed_box({0, 0, 0}, {6, 0.2f, 6}));
    r3::FitParams fp;
    r3::ensure_fitted(&floor, fp);
    r3::MeshData rest;
    r3::rest_pose_mesh(floor, &rest);
    p3::JoltWorld world;
    p3::BodyDesc ground;
    ground.motion = p3::Motion::Static;
    ground.layer = p3::kLayerStatic;
    p3::ShapeDesc mesh;
    mesh.type = p3::ShapeType::Mesh;
    for (const r3::Vertex3D& v : rest.vertices) mesh.points.push_back(v.position);
    mesh.indices = rest.indices;
    ground.shapes.push_back(std::move(mesh));
    if (!world.create(ground, &err)) return fail("mesh body: " + err);
    p3::BodyDesc crate;
    crate.motion = p3::Motion::Dynamic;
    crate.position = {0, 2.0f, 0};
    p3::ShapeDesc hull;
    hull.type = p3::ShapeType::ConvexHull;
    r3::Model crate_m = model_of(r3::make_cube(0.5f));
    r3::ensure_fitted(&crate_m, fp);
    if (crate_m.fitted.convex.empty()) return fail("crate hull");
    hull.points = crate_m.fitted.convex[0];
    crate.shapes.push_back(std::move(hull));
    const p3::BodyId id = world.create(crate, &err);
    if (!id) return fail("crate body: " + err);
    for (int i = 0; i < 180; ++i) world.step(1.0f / 60.0f);
    const r3::Vec3 at = world.position(id);
    if (at.y < 0.2f || at.y > 0.8f) return fail("crate fell through the mesh floor, y=" + std::to_string(at.y));
  }

  // Pieces: a sphere over the notch of the L settles on the base, not on a
  // single hull stretched across the gap.
  {
    p3::JoltWorld world;
    p3::BodyDesc body;
    body.motion = p3::Motion::Static;
    body.layer = p3::kLayerStatic;
    for (const auto& piece : lm.fitted.pieces) {
      p3::ShapeDesc s;
      s.type = p3::ShapeType::ConvexHull;
      s.points = piece;
      body.shapes.push_back(std::move(s));
    }
    if (!world.create(body, &err)) return fail("L body: " + err);
    p3::BodyDesc ball;
    ball.motion = p3::Motion::Dynamic;
    ball.position = {1.5f, 3.0f, 0.0f};
    p3::ShapeDesc sphere;
    sphere.type = p3::ShapeType::Sphere;
    sphere.size = {0.15f, 0, 0};
    ball.shapes.push_back(sphere);
    const p3::BodyId id = world.create(ball, &err);
    if (!id) return fail("ball: " + err);
    for (int i = 0; i < 240; ++i) world.step(1.0f / 60.0f);
    const r3::Vec3 at = world.position(id);
    // On the base or on a convex piece that bridges the lower notch, not
    // fallen through and not sitting on a single hull up at the upright.
    if (at.y > 1.3f || at.y < 0.4f || std::fabs(at.x - 1.5f) > 0.4f) {
      return fail("ball did not settle on the L, at " + std::to_string(at.x) + "," + std::to_string(at.y));
    }
  }

  // --- 2D: a rectangle traces as one polygon; a C as several. Box2D hits. --
  {
    std::vector<unsigned char> rgba(8 * 4 * 4, 255);
    const auto rect = ts::tombstone::runtime::trace_sprite_polygon(rgba.data(), 8, 4);
    if (rect.size() != 1 || rect[0].size() != 4) return fail("rectangle did not trace as one quad");
    std::vector<unsigned char> cshape(8 * 8 * 4, 0);
    auto paint = [&](int x, int y) {
      unsigned char* p = cshape.data() + (static_cast<std::size_t>(y) * 8 + x) * 4;
      p[0] = p[1] = p[2] = p[3] = 255;
    };
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x)
        if (y < 2 || y >= 6 || x < 3) paint(x, y);
    const auto pieces = ts::tombstone::runtime::trace_sprite_polygon(cshape.data(), 8, 8);
    if (pieces.size() < 2) return fail("concave sprite stayed one polygon");

    p2::Box2DWorld world({0, 400.0f}, 32.0f);
    p2::BodyDesc floor;
    floor.motion = p2::Motion::Static;
    floor.position = {32, 80};
    p2::ShapeDesc poly;
    poly.type = p2::ShapeType::Polygon;
    for (const auto& pt : rect[0]) poly.points.push_back({pt.first * 8.0f - 32.0f, pt.second * 8.0f - 16.0f});
    floor.shapes.push_back(poly);
    if (!world.create(floor, &err)) return fail("2d floor: " + err);
    p2::BodyDesc drop;
    drop.motion = p2::Motion::Dynamic;
    drop.position = {32, 10};
    p2::ShapeDesc box;
    box.type = p2::ShapeType::Box;
    box.w = 8;
    box.h = 8;
    drop.shapes.push_back(box);
    const p2::BodyId id = world.create(drop, &err);
    if (!id) return fail("2d box: " + err);
    for (int i = 0; i < 180; ++i) world.step(1.0f / 60.0f);
    const float y = world.position(id).y;
    // Floor centre is y=80, the polygon is 32 px tall (4 * 8) so its top is
    // about y=64. The box is 8 px tall, so its centre stops near 60.
    if (y < 50.0f || y > 75.0f) return fail("sprite polygon did not stop the box, y=" + std::to_string(y));
  }

  std::cout << "[smoke] mesh parts: split, undo, oriented box, convex pieces, mesh and sprite colliders\n";
  fs::remove_all(dir, ec);
  return 0;
}
