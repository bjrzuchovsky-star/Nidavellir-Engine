// Headless gravity pile: ts_admin --bench-physics=N [--bench-model=<path>]
//
// Three built-in scenes, plus the caller's model when --bench-model is set:
//   simple   unit boxes, primitive cube (12 triangles)
//   crate    1,682-tri mesh, 4.66 x 4.64 x 4.60 m box collider (crate.blend)
//   dense    ~10k-tri mesh, auto-fit box collider
//
// FrameStats::triangles is the main color pass only. Shadow cascades are
// counted separately and were not in that number, which is how a 2.7M-tri
// pile shows up as ~16M in a GPU capture (main + several shadow passes).

#include "SmokePhysics.h"

#include "plugins/split/Split.h"
#include "render3d/ColliderFit.h"
#include "render3d/ModelImport.h"
#include "render3d/SceneView.h"
#include "world3d/World3D.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace s3 = ts::tombstone::scene3d;
namespace r3 = ts::tombstone::render3d;
namespace w3 = ts::tombstone::world3d;
namespace fs = std::filesystem;

namespace {

using clock = std::chrono::steady_clock;

double ms_since(clock::time_point a) {
  return std::chrono::duration<double, std::milli>(clock::now() - a).count();
}

struct BuiltMesh {
  r3::MeshData mesh;
  r3::Vec3 size{1, 1, 1};
};

void add_grid_face(r3::MeshData* m, int cols, int rows, r3::Vec3 origin, r3::Vec3 axis_u, r3::Vec3 axis_v) {
  const auto base = static_cast<std::uint32_t>(m->vertices.size());
  const r3::Vec3 n = r3::normalize(r3::cross(axis_u, axis_v));
  for (int y = 0; y <= rows; ++y) {
    for (int x = 0; x <= cols; ++x) {
      const float fx = static_cast<float>(x) / static_cast<float>(cols);
      const float fy = static_cast<float>(y) / static_cast<float>(rows);
      r3::Vertex3D v;
      v.position = origin + axis_u * fx + axis_v * fy;
      v.normal = n;
      v.u = fx;
      v.v = fy;
      m->vertices.push_back(v);
    }
  }
  auto id = [&](int x, int y) { return base + static_cast<std::uint32_t>(y * (cols + 1) + x); };
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x) {
      m->indices.insert(m->indices.end(), {id(x, y), id(x + 1, y), id(x + 1, y + 1), id(x, y), id(x + 1, y + 1), id(x, y + 1)});
    }
  }
}

BuiltMesh make_box_mesh(r3::Vec3 size, int cols, int rows, int cols_last, int rows_last) {
  BuiltMesh b;
  b.size = size;
  const r3::Vec3 h = size * 0.5f;
  const r3::Vec3 x{size.x, 0, 0}, y{0, size.y, 0}, z{0, 0, size.z};
  add_grid_face(&b.mesh, cols, rows, {-h.x, -h.y, -h.z}, x, y);
  add_grid_face(&b.mesh, cols, rows, {-h.x, -h.y, h.z}, x, y);
  add_grid_face(&b.mesh, cols, rows, {-h.x, -h.y, -h.z}, z, y);
  add_grid_face(&b.mesh, cols, rows, {h.x, -h.y, -h.z}, z, y);
  add_grid_face(&b.mesh, cols, rows, {-h.x, h.y, -h.z}, x, z);
  add_grid_face(&b.mesh, cols_last, rows_last, {-h.x, -h.y, -h.z}, x, z);
  return b;
}

bool write_obj(const fs::path& path, const r3::MeshData& mesh) {
  std::ofstream out(path);
  if (!out) return false;
  for (const r3::Vertex3D& v : mesh.vertices) {
    out << "v " << v.position.x << ' ' << v.position.y << ' ' << v.position.z << '\n';
  }
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    out << "f " << (mesh.indices[i] + 1) << ' ' << (mesh.indices[i + 1] + 1) << ' ' << (mesh.indices[i + 2] + 1)
        << '\n';
  }
  return static_cast<bool>(out);
}

struct Case {
  std::string name;
  std::string mesh_source;  // project-relative, or primitive:
  r3::Vec3 size{1, 1, 1};
  int tris = 12;
  std::string collider = "box";
  std::string note;
};

s3::Entity3D make_body(std::uint64_t id, const std::string& name, r3::Vec3 pos, const Case& c, bool dynamic) {
  s3::Entity3D e;
  e.id = id;
  e.name = name;
  e.transform.position = pos;
  e.mesh = s3::MeshRef{c.mesh_source, true};
  e.collider = s3::Collider3D{};
  e.collider->shape = s3::ColliderShape3D::Box;
  e.collider->size = c.size;
  e.collider->fit = "auto";
  e.rigid_body = s3::RigidBody3D{};
  e.rigid_body->motion = dynamic ? s3::RigidBody3D::Motion::Dynamic : s3::RigidBody3D::Motion::Static;
  return e;
}

// The old update_collisions() built a transform matrix inside the inner
// loop (collider_box), so the cost is not the AABB test. This repeats that.
double legacy_pairs_ms(const std::vector<s3::Entity3D>& ents) {
  const auto t0 = clock::now();
  int hits = 0;
  auto box_of = [](const s3::Entity3D& e) {
    const r3::Vec3 h = e.collider->size * 0.5f;
    r3::Aabb local{-h, h};
    local.min = local.min + e.collider->offset;
    local.max = local.max + e.collider->offset;
    return local.transformed(e.transform.matrix());
  };
  for (std::size_t i = 0; i < ents.size(); ++i) {
    if (!ents[i].collider || ents[i].collider->trigger) continue;
    const r3::Aabb ab = box_of(ents[i]);
    for (std::size_t j = i + 1; j < ents.size(); ++j) {
      if (!ents[j].collider || ents[j].collider->trigger) continue;
      const r3::Aabb bb = box_of(ents[j]);
      const bool hit = ab.min.x < bb.max.x && ab.max.x > bb.min.x && ab.min.y < bb.max.y && ab.max.y > bb.min.y &&
                       ab.min.z < bb.max.z && ab.max.z > bb.min.z;
      hits += hit ? 1 : 0;
    }
  }
  if (hits == -1) std::cerr << "unreachable\n";
  return ms_since(t0);
}

double legacy_hierarchy_ms(const std::vector<s3::Entity3D>& ents) {
  const auto t0 = clock::now();
  int kids = 0;
  for (const s3::Entity3D& e : ents) {
    for (const s3::Entity3D& c : ents) kids += c.parent == e.id ? 1 : 0;
  }
  if (kids == -1) std::cerr << "unreachable\n";
  return ms_since(t0);
}

double legacy_lookup_ms(const std::vector<std::uint64_t>& ids) {
  const auto t0 = clock::now();
  int found = 0;
  for (std::uint64_t id : ids) {
    for (std::uint64_t other : ids) {
      if (other == id) {
        ++found;
        break;
      }
    }
  }
  if (found < 0) std::cerr << "unreachable\n";
  return ms_since(t0);
}

void hierarchy_ms(int n, double* unclipped, double* clipped) {
  *unclipped = *clipped = -1;
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGuiContext* ctx = ImGui::CreateContext();
  ImGui::SetCurrentContext(ctx);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(320, 720);
  io.IniFilename = nullptr;
  io.DeltaTime = 1.0f / 60.0f;
  unsigned char* px = nullptr;
  int fw = 0, fh = 0;
  io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
  auto rows = [&](int begin, int end) {
    for (int i = begin; i < end; ++i) {
      ImGui::PushID(i);
      ImGui::SmallButton("o");
      ImGui::SameLine();
      ImGui::TreeNodeEx("row", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                        "Crate %d", i);
      ImGui::PopID();
    }
  };
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(320, 720));
  ImGui::Begin("Hierarchy");
  const auto t0 = clock::now();
  rows(0, n);
  *unclipped = ms_since(t0);
  ImGui::End();
  ImGui::Render();

  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(320, 720));
  ImGui::Begin("Hierarchy");
  const auto t1 = clock::now();
  ImGuiListClipper clipper;
  clipper.Begin(n);
  while (clipper.Step()) rows(clipper.DisplayStart, clipper.DisplayEnd);
  *clipped = ms_since(t1);
  ImGui::End();
  ImGui::Render();
  ImGui::DestroyContext(ctx);
  ImGui::SetCurrentContext(prev);
}

void print_render(const char* label, r3::SceneView& view, const std::vector<s3::Entity3D>& ents, const r3::Camera3D& cam,
                  const r3::Environment& env, int cascades) {
  const auto t0 = clock::now();
  view.begin(env, ents, cam, 1280, 720);
  const r3::FrameStats st = view.end();
  const double cpu = ms_since(t0);
  const r3::SceneView::SubmitStats& sub = view.last_submit();
  const std::size_t shadow_total = sub.shadow_triangles * static_cast<std::size_t>(std::max(cascades, 0));
  std::cout << "  render " << label << "  cpu " << cpu << " ms  draws " << sub.draws
            << " (renderer " << st.draws << ")  visible " << sub.visible << "  culled " << sub.culled
            << "  lod full/mid/far " << sub.lod_full << '/' << sub.lod_mid << '/' << sub.lod_far << '\n';
  std::cout << "    main-pass triangles " << sub.main_triangles
            << "   (FrameStats.triangles " << st.triangles << ", main pass only)\n";
  std::cout << "    shadow triangles, one cascade " << sub.shadow_triangles << "  x" << cascades
            << " cascades = " << shadow_total << "\n";
  std::cout << "    GPU triangles if every cascade draws (main + shadows) " << (sub.main_triangles + shadow_total)
            << '\n';
}

int run_case(const Case& c, int n, const fs::path& project) {
  std::cout << "\n== " << c.name << "  n=" << n << "  tris/object=" << c.tris
            << "  collider=" << c.collider << " ==" << '\n';
  if (!c.note.empty()) std::cout << "  " << c.note << '\n';
  const std::size_t source = static_cast<std::size_t>(c.tris) * static_cast<std::size_t>(n);
  std::cout << "  source triangles (mesh x copies, not times passes) " << source << '\n';

  s3::Scene3D scene;
  scene.entities.push_back(make_body(1, "Floor", {0, -0.5f, 0},
                                     Case{"floor", "primitive:cube", {400, 1, 400}, 12, "box", ""}, false));
  const float gap = 0.08f;
  const float px = c.size.x + gap, py = c.size.y + gap, pz = c.size.z + gap;
  int layers = 4;
  if (n < 4) layers = 1;
  int per = (n + layers - 1) / layers;
  int cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(per)))));
  int rows = (per + cols - 1) / cols;
  int made = 0;
  for (int layer = 0; layer < layers && made < n; ++layer) {
    for (int z = 0; z < rows && made < n; ++z) {
      for (int x = 0; x < cols && made < n; ++x) {
        const r3::Vec3 pos{(x - cols * 0.5f) * px, c.size.y * 0.5f + 0.05f + layer * py, (z - rows * 0.5f) * pz};
        scene.entities.push_back(make_body(static_cast<std::uint64_t>(made + 2), "Body" + std::to_string(made), pos, c, true));
        ++made;
      }
    }
  }

  w3::World3D world;
  std::string err;
  if (!world.start(scene, project.string(), &err)) {
    std::cerr << "  start failed: " << err << '\n';
    return 1;
  }
  std::cout << "  bodies " << world.physics_bodies() << '\n';

  w3::Input3D input;
  for (int i = 0; i < 3; ++i) world.step(input);

  w3::World3D::TickTimes acc;
  ts::tombstone::physics3d::World::StepProfile jolt{};
  const int kSteps = 60;
  const auto wall0 = clock::now();
  int steps = 0;
  for (; steps < kSteps && ms_since(wall0) < 8000.0; ++steps) {
    world.step(input);
    const auto& t = world.tick_times();
    acc.pre_ms += t.pre_ms;
    acc.physics_ms += t.physics_ms;
    acc.post_ms += t.post_ms;
    acc.pairs_ms += t.pairs_ms;
    acc.rebuild_ms += t.rebuild_ms;
    acc.total_ms += t.total_ms;
    if (world.physics()) {
      const auto p = world.physics()->step_profile();
      jolt.broadphase_ms += p.broadphase_ms;
      jolt.narrowphase_ms += p.narrowphase_ms;
      jolt.solver_ms += p.solver_ms;
      jolt.other_ms += p.other_ms;
      if (steps == 2) {
        for (int j = 0; j < p.job_count; ++j) {
          std::cout << "    jolt job " << (p.jobs[j].name ? p.jobs[j].name : "?") << " " << p.jobs[j].ms << " ms\n";
        }
      }
    }
  }
  const double wall = ms_since(wall0);
  const double inv = steps > 0 ? 1.0 / steps : 0;
  std::cout << "  steps " << steps << " in " << wall << " ms  (" << (wall / std::max(steps, 1)) << " ms/step wall)\n";
  std::cout << "  physics step " << acc.physics_ms * inv << " ms   broadphase " << jolt.broadphase_ms * inv
            << "  narrowphase " << jolt.narrowphase_ms * inv << "  solver " << jolt.solver_ms * inv
            << "  other " << jolt.other_ms * inv << '\n';
  std::cout << "  sync pre " << acc.pre_ms * inv << " ms  post " << acc.post_ms * inv << " ms  rebuild "
            << acc.rebuild_ms * inv << " ms\n";
  std::cout << "  script pair broadphase " << acc.pairs_ms * inv << " ms\n";

  int awake = 0, dynamic = 0;
  std::vector<std::uint64_t> ids;
  ids.reserve(static_cast<std::size_t>(n));
  for (const s3::Entity3D& e : world.entities()) {
    if (!e.rigid_body || e.rigid_body->motion != s3::RigidBody3D::Motion::Dynamic) continue;
    ++dynamic;
    const auto body = world.body_of(e.id);
    if (body && world.physics() && world.physics()->awake(body)) ++awake;
    ids.push_back(e.id);
  }
  std::cout << "  awake " << awake << " / " << dynamic << " dynamic\n";
  double legacy_pair = 0, legacy_lookup = 0, legacy_hier = 0;
  if (world.entities().size() <= 6000) {
    legacy_pair = legacy_pairs_ms(world.entities());
    legacy_lookup = legacy_lookup_ms(ids);
    legacy_hier = legacy_hierarchy_ms(world.entities());
    std::cout << "  legacy collision (matrix per pair, old update_collisions) " << legacy_pair << " ms\n";
    std::cout << "  legacy linear entity lookup " << legacy_lookup << " ms\n";
    std::cout << "  legacy hierarchy child scan " << legacy_hier << " ms\n";
  }

  auto renderer = r3::make_null_renderer3d();
  std::string rerr;
  if (!renderer->init(&rerr)) {
    std::cerr << "  null renderer: " << rerr << '\n';
    return 1;
  }
  r3::SceneView view(renderer.get());
  view.set_project_dir(project.string());
  r3::Camera3D cam;
  cam.target = {0, c.size.y * 2.0f, 0};
  cam.position = {0, c.size.y * 6.0f + 8.0f, std::max(30.0f, cols * px * 0.7f)};
  cam.fov_y_degrees = 60.0f;
  r3::Environment on;
  on.shadows = true;
  on.shadow_distance = std::max(80.0f, cols * px);
  on.lighting.preset = 0;
  view.begin(on, world.entities(), cam, 1280, 720);
  view.end();
  print_render("lighting on, classic (1 cascade, shadows cover the pile)", view, world.entities(), cam, on, 1);
  std::cout << "    cinematic preset is 4 cascades: multiply the one-cascade shadow line by 4\n";
  std::cout << "    1,682 x 1,600 = 2,691,200 source. Main + 4 cascades = 13,456,000. A 6th geometry\n";
  std::cout << "    pass (extra depth or a second shadow) is 16,147,200, which matches a 16M GPU capture.\n";
  r3::Environment off = on;
  off.shadows = false;
  off.lighting.cascades = 0;
  print_render("lighting off (no shadow pass)", view, world.entities(), cam, off, 0);

  double ui_all = 0, ui_clip = 0;
  hierarchy_ms(n, &ui_all, &ui_clip);
  std::cout << "  editor hierarchy, every row " << ui_all << " ms;  clipped to the window " << ui_clip << " ms\n";

  const double legacy = legacy_pair + legacy_lookup + legacy_hier + acc.physics_ms * inv + acc.pre_ms * inv + acc.post_ms * inv + ui_all;
  const double now = acc.total_ms * inv + ui_clip;
  std::cout << "  CPU frame estimate (physics + pair scan or current step + hierarchy), no GPU:\n";
  std::cout << "    before ~" << (legacy > 0.01 ? 1000.0 / legacy : 0) << " FPS   (" << legacy << " ms)\n";
  std::cout << "    after  ~" << (now > 0.01 ? 1000.0 / now : 0) << " FPS   (" << now << " ms)\n";
  world.stop();
  return 0;
}

}  // namespace

int run_physics_bench(int argc, char** argv) {
  int n = 0;
  std::string model;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a.rfind("--bench-physics=", 0) == 0) n = std::atoi(a.c_str() + 16);
    else if (a == "--bench-physics" && i + 1 < argc) n = std::atoi(argv[++i]);
    else if (a.rfind("--bench-model=", 0) == 0) model = a.substr(14);
    else if (a == "--bench-model" && i + 1 < argc) model = argv[++i];
  }
  if (n < 1 || n > 20000) {
    std::cerr << "usage: ts_admin --bench-physics=N [--bench-model=<file.blend/.fbx/.glb/.obj>]\n";
    std::cerr << "N is 1..20000 copies (1600 and 5000 are the reported sizes).\n";
    return 2;
  }

  ts::tombstone::plugins::ProviderHold hold(ts::tombstone::plugins::split_plugin_ids());
  const fs::path project = fs::temp_directory_path() / "ts-phys-bench";
  std::error_code ec;
  fs::create_directories(project, ec);

  const BuiltMesh crate = make_box_mesh({4.66f, 4.64f, 4.60f}, 10, 14, 3, 47);
  const BuiltMesh dense = make_box_mesh({1.2f, 1.2f, 1.2f}, 29, 29, 29, 29);
  const int crate_tris = static_cast<int>(crate.mesh.indices.size() / 3);
  const int dense_tris = static_cast<int>(dense.mesh.indices.size() / 3);
  if (!write_obj(project / "crate.obj", crate.mesh) || !write_obj(project / "dense.obj", dense.mesh)) {
    std::cerr << "could not write bench meshes\n";
    return 1;
  }
  std::cout << "Nidavellir physics bench  n=" << n << "\n";
  std::cout << "Triangle counter: FrameStats.triangles is the main color pass only.\n";
  std::cout << "Shadow draws used to increment shadow_casters and not triangles.\n";
  std::cout << "crate mesh " << crate_tris << " tris, " << crate.mesh.vertices.size() << " verts, size 4.66 x 4.64 x 4.60\n";
  std::cout << "dense mesh " << dense_tris << " tris\n";

  int rc = 0;
  Case simple{"simple", "primitive:cube", {1, 1, 1}, 12, "box (primitive)", "unit boxes"};
  Case crate_case{"crate", "crate.obj", {4.66f, 4.64f, 4.60f}, crate_tris, "box (auto-fit, same as import)",
                  "1682-tri stand-in for crate.blend; dynamic body, box collider"};
  Case dense_case{"dense", "dense.obj", {1.2f, 1.2f, 1.2f}, dense_tris, "box (auto-fit)",
                  "~10k-tri mesh, box collider, not a triangle mesh"};
  rc |= run_case(simple, n, project);
  rc |= run_case(crate_case, n, project);
  rc |= run_case(dense_case, n, project);

  if (!model.empty()) {
    r3::Model loaded;
    std::string err;
    if (!r3::load_model(model, &loaded, &err)) {
      std::cerr << "bench model failed: " << err << '\n';
      return 1;
    }
    s3::Collider3D col;
    r3::fit_collider(loaded, r3::ColliderFit::Auto, &col);
    const fs::path src(model);
    Case mcase;
    mcase.name = "model " + src.filename().string();
    mcase.mesh_source = model;
    mcase.size = col.size.x > 0 ? col.size : loaded.bounds.extent();
    mcase.tris = static_cast<int>(loaded.triangles());
    mcase.collider = std::string(s3::collider_shape_name(col.shape)) + " (auto-fit default)";
    mcase.note = "imported through load_model; " + std::to_string(loaded.triangles()) + " tris, bounds " +
                 std::to_string(loaded.bounds.extent().x) + " x " + std::to_string(loaded.bounds.extent().y) + " x " +
                 std::to_string(loaded.bounds.extent().z);
    rc |= run_case(mcase, n, project);
  }
  fs::remove_all(project, ec);
  return rc;
}
