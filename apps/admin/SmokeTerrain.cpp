#include "SmokeTerrain.h"

#include "editor/ProjectInfo.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "fx/Particles.h"
#include "physics3d/Physics3D.h"
#include "plugins/weather/WeatherPlugin.h"
#include "render3d/Renderer3D.h"
#include "render3d/Scene3D.h"
#include "render3d/SceneView.h"
#include "runtime/World.h"
#include "terrain/Foliage.h"
#include "terrain/Terrain.h"
#include "terrain/TerrainComponent.h"
#include "terrain/TerrainView.h"
#include "weather/Weather.h"
#include "weather/Weather3D.h"
#include "world3d/World3D.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace ts::tombstone;
namespace fs = std::filesystem;
namespace r3 = ts::tombstone::render3d;
namespace s3 = ts::tombstone::scene3d;
namespace w3 = ts::tombstone::world3d;
namespace tr = ts::tombstone::terrain;
namespace wx = ts::tombstone::weather;

const char* g_what = "terrain";

int fail(const std::string& m) {
  std::cerr << "[smoke] " << g_what << " FAILED: " << m << '\n';
  return 1;
}

bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

fs::path temp_project(const char* name) {
  const fs::path dir = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "scripts");
  return dir;
}

void write_file(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

// ---- Terrain -------------------------------------------------------------------

int brushes() {
  tr::TerrainData t;
  tr::reset(t, 65, 64.0f);
  if (!t.valid() || !near(t.cell(), 1.0f, 1e-5f)) return fail("reset 65 x 64 m");
  if (tr::valid_resolution(64) || !tr::valid_resolution(129)) return fail("valid_resolution");
  tr::Brush b;
  b.op = tr::BrushOp::Raise;
  b.radius = 6;
  b.strength = 4;
  b.hardness = 0.5f;
  const std::uint64_t rev0 = t.revision;
  for (int i = 0; i < 30; ++i) tr::apply_brush(t, b, 0, 0, 1.0f / 30.0f);
  const float top = tr::height_at(t, 0, 0);
  if (!(top > 3.0f && top < 4.5f)) return fail("raise ~4 m in a second, got " + std::to_string(top));
  if (!near(tr::height_at(t, 20, 20), 0.0f, 1e-5f)) return fail("raise stays inside the radius");
  if (t.revision == rev0) return fail("revision bumps");
  tr::Rect dirty;
  bool splat = false, fol = false;
  if (!tr::edits_since(t, rev0, &dirty, &splat, &fol) || dirty.empty() || dirty.i0 < 24 || dirty.i1 > 40)
    return fail("edit log covers just the brush");
  b.op = tr::BrushOp::Lower;
  b.strength = 2;
  tr::apply_brush(t, b, 0, 0, 0.5f);
  if (!(tr::height_at(t, 0, 0) < top - 0.5f)) return fail("lower");
  // Smooth knocks a one-vertex spike down.
  t.h(32, 32) += 5.0f;
  const float spike = t.h(32, 32) - t.h(34, 32);
  b.op = tr::BrushOp::Smooth;
  b.radius = 3;
  b.strength = 4;
  for (int i = 0; i < 10; ++i) tr::apply_brush(t, b, 0, 0, 0.1f);
  if (!(t.h(32, 32) - t.h(34, 32) < spike * 0.5f)) return fail("smooth");
  b.radius = 6;
  // Flatten towards a target.
  b.op = tr::BrushOp::Flatten;
  b.target = 1.0f;
  b.hardness = 1.0f;
  b.strength = 20;
  for (int i = 0; i < 30; ++i) tr::apply_brush(t, b, 0, 0, 0.1f);
  if (!near(tr::height_at(t, 0, 0), 1.0f, 0.05f) || !near(tr::height_at(t, 3, 2), 1.0f, 0.1f))
    return fail("flatten to 1 m, got " + std::to_string(tr::height_at(t, 0, 0)));
  // Noise is deterministic per seed.
  tr::TerrainData n1 = t, n2 = t;
  b.op = tr::BrushOp::Noise;
  b.strength = 3;
  b.seed = 7;
  tr::apply_brush(n1, b, 10, 10, 0.2f);
  tr::apply_brush(n2, b, 10, 10, 0.2f);
  if (n1.heights != n2.heights || n1.heights == t.heights) return fail("noise deterministic + changes");
  // Paint layer 2 (rock): weights move, sum stays 255.
  b.op = tr::BrushOp::Paint;
  b.layer = 2;
  b.strength = 8;
  b.hardness = 1.0f;
  for (int i = 0; i < 20; ++i) tr::apply_brush(t, b, -20, -20, 0.1f);
  float w[tr::kLayers];
  tr::weights_at(t, -20, -20, w);
  if (!(w[2] > 0.9f)) return fail("paint layer 3 dominates");
  const std::size_t k = (static_cast<std::size_t>(12) * t.res + 12) * 4;
  const int sum = t.splat[k] + t.splat[k + 1] + t.splat[k + 2] + t.splat[k + 3];
  if (sum < 250 || sum > 258) return fail("splat sum ~255, got " + std::to_string(sum));
  // Raycast from above.
  float hit = 0;
  if (!tr::raycast(t, {0, 50, 0}, {0, -1, 0}, 100, &hit) || !near(50 - hit, 1.0f, 0.06f)) return fail("raycast");
  if (tr::raycast(t, {0, 50, 0}, {0, 1, 0}, 100, &hit)) return fail("raycast up misses");
  std::cout << "[smoke] terrain brushes OK (raise " << top << " m, lower, smooth, flatten, noise, paint, raycast)\n";
  return 0;
}

int lod_and_files(const fs::path& dir) {
  tr::TerrainData t;
  tr::reset(t, 129, 128.0f);
  tr::generate_hills(t, 3, 8.0f, 32.0f);
  const tr::ChunkLayout l = tr::chunk_layout(t, 32);
  if (l.count != 4 || l.lods < 3) return fail("4 x 4 chunks, 3+ LODs");
  const r3::MeshData m0 = tr::build_chunk(t, l, 1, 1, 0, 1.0f);
  const r3::MeshData m2 = tr::build_chunk(t, l, 1, 1, 2, 1.0f);
  if (m0.indices.size() <= m2.indices.size() * 8) return fail("LOD 2 has ~1/16 the triangles");
  if (m0.vertices.empty()) return fail("chunk vertices");
  float d[4] = {45, 90, 180, 360};
  if (tr::select_lod(10, d, 3, 4) != 0 || tr::select_lod(100, d, 3, 4) != 2 || tr::select_lod(1000, d, 3, 4) != 3)
    return fail("select_lod");
  const r3::Aabb bb = tr::chunk_bounds(t, l, 0, 0);
  if (!(bb.max.y > bb.min.y)) return fail("chunk bounds height");
  // Foliage in the file too.
  std::vector<tr::FoliageType> types(1);
  tr::foliage_preset("pine", &types[0]);
  tr::FoliageBrush fb;
  fb.radius = 20;
  fb.fill = 1;
  tr::paint_foliage(t, types, fb, 0, 0);
  const std::string bytes = tr::encode(t);
  tr::TerrainData back;
  std::string err;
  if (!tr::decode(bytes, &back, &err)) return fail("decode: " + err);
  if (back.heights != t.heights || back.splat != t.splat || back.foliage != t.foliage || back.res != 129)
    return fail("encode / decode round trip");
  if (tr::decode(bytes.substr(0, 20), &back, &err)) return fail("truncated file must fail");
  // Scene3D v16 round trip with a terrain component.
  s3::Scene3D s;
  s3::Entity3D e;
  e.id = 1;
  e.name = "Terrain";
  tr::Terrain3D c;
  c.data = "terrain/smoke.tsterrain";
  c.layers[3].texture = "builtin:snow";
  c.layers[1].tiling = 7.5f;
  c.foliage = types;
  c.lod_distance = 60;
  e.terrain = c;
  s.entities.push_back(e);
  s.environment.fog_density = 0.02f;
  s.environment.wind_strength = 0.4f;
  const std::string path = (dir / "scene3d.json").string();
  if (!s3::save_file(path, s, &err)) return fail(err);
  s3::Scene3D in;
  if (!s3::load_file(path, &in, &err)) return fail(err);
  if (s3::kScene3DVersion < 16) return fail("Scene3D version 16+");
  if (in.entities.size() != 1 || !in.entities[0].terrain || !(*in.entities[0].terrain == c))
    return fail("Scene3D terrain round trip");
  if (!near(in.environment.fog_density, 0.02f, 1e-5f) || !near(in.environment.wind_strength, 0.4f, 1e-5f))
    return fail("fog + wind round trip");
  std::cout << "[smoke] terrain LOD + files OK (" << l.count << "x" << l.count << " chunks, " << l.lods
            << " LODs, " << m0.indices.size() / 3 << " -> " << m2.indices.size() / 3
            << " tris, .tsterrain " << bytes.size() << " bytes, Scene3D v16)\n";
  return 0;
}

int foliage_checks() {
  tr::TerrainData t;
  tr::reset(t, 65, 64.0f);
  std::vector<tr::FoliageType> types(2);
  tr::foliage_preset("grass", &types[0]);
  tr::foliage_preset("rock", &types[1]);
  types[0].density = 2.0f;
  tr::FoliageBrush b;
  b.type = 0;
  b.radius = 6;
  b.fill = 1.0f;
  b.seed = 11;
  const int added = tr::paint_foliage(t, types, b, 0, 0);
  const float expect = 3.14159f * 36 * 2.0f;
  if (added < expect * 0.6f || added > expect * 1.3f) return fail("paint density ~" + std::to_string(expect) +
                                                                  ", got " + std::to_string(added));
  if (tr::paint_foliage(t, types, b, 0, 0) != 0) return fail("painting the same spot again adds nothing");
  tr::TerrainData t2;
  tr::reset(t2, 65, 64.0f);
  tr::paint_foliage(t2, types, b, 0, 0);
  if (t2.foliage != t.foliage) return fail("paint deterministic");
  b.type = 1;
  tr::paint_foliage(t, types, b, 15, 15);
  const int rocks = tr::count_foliage(t, 1);
  if (rocks <= 0) return fail("rocks painted");
  tr::FoliageBrush e;
  e.type = 0;
  e.radius = 3;
  e.fill = 1;
  const int erased = tr::erase_foliage(t, e, 0, 0);
  if (erased <= 0 || tr::count_foliage(t, 1) != rocks) return fail("erase only grass in the brush");
  // Sculpting refits foliage to the ground.
  tr::Brush r;
  r.radius = 10;
  r.strength = 5;
  const tr::Rect dirty = tr::apply_brush(t, r, 15, 15, 0.5f);
  tr::refit_foliage(t, dirty);
  for (const tr::FoliageInstance& f : t.foliage)
    if (!near(f.y, tr::height_at(t, f.x, f.z), 1e-3f)) return fail("foliage follows sculpting");
  tr::remove_foliage_type(t, 0);
  if (tr::count_foliage(t, 0) != rocks) return fail("remove type shifts indices");
  // Wind: calm = no bend; mesh generators exist for every builtin.
  tr::Wind calm;
  calm.strength = 0;
  calm.gust = 0;
  if (!near(tr::wind_bend(3, 4, 1.5f, calm), 0.0f, 1e-6f)) return fail("calm wind bends nothing");
  tr::Wind breeze;
  if (!(std::fabs(tr::wind_bend(3, 4, 1.5f, breeze)) > 0.0f)) return fail("a breeze bends");
  for (const std::string& n : tr::foliage_mesh_names()) {
    r3::MeshData m;
    if (!tr::make_foliage_mesh(n, &m) || m.indices.empty()) return fail("foliage mesh " + n);
  }
  std::cout << "[smoke] foliage OK (" << added << " grass painted deterministically, " << erased
            << " erased, " << rocks << " rocks kept, refit, wind, " << tr::foliage_mesh_names().size()
            << " meshes)\n";
  return 0;
}

s3::Scene3D hill_scene(const fs::path& dir) {
  tr::Terrain3D c;
  c.data = "terrain/hill.tsterrain";
  c.resolution = 65;
  c.size = 64;
  tr::FoliageType pine, grass;
  tr::foliage_preset("pine", &pine);
  tr::foliage_preset("grass", &grass);
  c.foliage = {grass, pine};
  tr::TerrainData t;
  tr::reset(t, 65, 64);
  tr::Brush b;
  b.radius = 12;
  b.strength = 6;
  b.hardness = 0.3f;
  for (int i = 0; i < 20; ++i) tr::apply_brush(t, b, 10, 0, 0.05f);  // ~6 m hill at x=10
  tr::FoliageBrush fb;
  fb.type = 0;
  fb.radius = 8;
  fb.fill = 1;
  tr::paint_foliage(t, c.foliage, fb, -10, -10);
  fb.type = 1;
  fb.radius = 5;
  tr::paint_foliage(t, c.foliage, fb, -15, 15);
  std::string err;
  fs::create_directories((dir / c.data).parent_path());
  tr::save_file((dir / c.data).string(), t, &err);
  s3::Scene3D s;
  s3::Entity3D e;
  e.id = 1;
  e.name = "Terrain";
  e.terrain = c;
  s.entities.push_back(e);
  s3::Entity3D rider;
  rider.id = 2;
  rider.name = "Rider";
  rider.transform.position = {10, 9, 0};
  s3::Collider3D col;
  col.shape = s3::Collider3D::Shape::Capsule;
  col.size = {0.4f, 2.0f, 0.4f};
  rider.collider = col;
  rider.player = s3::Player3D{};
  s.entities.push_back(rider);
  s3::Entity3D probe;
  probe.id = 3;
  probe.name = "Probe";
  probe.script = ts::tombstone::ScriptData{};
  probe.script->path = "scripts/probe.lua";
  s.entities.push_back(probe);
  write_file(dir / "scripts/probe.lua", R"(
function on_start()
  local h = terrain.height(10, 0)
  self:set("h", h or -1)
  local nx, ny, nz = terrain.normal(-20, -20)
  self:set("ny", ny or -1)
  local layer, w = terrain.layer(-20, -20)
  self:set("layer", layer or -1)
  self:set("pines", terrain.foliage(-15, 15, 6))
  self:set("outside", terrain.height(500, 500) == nil and 1 or 0)
end
)");
  s.environment.wind_strength = 0.3f;
  return s;
}

int world_checks(const fs::path& dir) {
  const s3::Scene3D s = hill_scene(dir);
  tr::forget();
  w3::World3D w;
  std::string err;
  if (!w.start(s, dir.string(), &err)) return fail("start: " + err);
  float y = 0;
  if (!w.terrain_height(10, 0, &y) || !(y > 4.0f)) return fail("World3D terrain_height on the hill");
  // Jolt heightfield: a ray down lands on the hill.
  ts::tombstone::physics3d::RayHit hit;
  if (!w.phys_raycast({10, 30, 0}, {0, -1, 0}, 60, &hit) || !near(hit.point.y, y, 0.15f))
    return fail("Jolt heightfield raycast (" + std::to_string(hit.point.y) + " vs " + std::to_string(y) + ")");
  if (!w.phys_raycast({-15, 30, 15}, {0, -1, 0}, 60, &hit)) return fail("ray at the pines");
  // The rider drops onto the hill and stands there.
  w.run_ticks(120, {});
  const r3::Vec3 p = w.find(2)->transform.position;
  float ground = 0;
  w.terrain_height(p.x, p.z, &ground);
  if (!w.grounded() || !(p.y > ground - 0.2f && p.y < ground + 1.6f))
    return fail("rider stands on the terrain (y " + std::to_string(p.y) + ", ground " + std::to_string(ground) + ")");
  auto num = [&](const char* k) {
    const auto* v = w.state(3, k);
    return v ? v->number : -99.0;
  };
  if (!near(static_cast<float>(num("h")), y, 0.01f) || !(num("ny") > 0.9) || num("layer") != 1 ||
      !(num("pines") > 0) || num("outside") != 1)
    return fail("Lua terrain.* (h " + std::to_string(num("h")) + ", layer " + std::to_string(num("layer")) +
                ", pines " + std::to_string(num("pines")) + ", ny " + std::to_string(num("ny")) + ", outside " + std::to_string(num("outside")) + ")");
  w.stop();
  // Null renderer: SceneView draws chunks + foliage and counts instances.
  std::unique_ptr<r3::Renderer3D> nr = r3::make_null_renderer3d();
  nr->init(nullptr);
  r3::SceneView view(nr.get());
  view.set_project_dir(dir.string());
  r3::Camera3D cam;
  cam.position = {-10, 8, 10};
  cam.target = {-10, 0, -10};
  view.begin(s.environment, s.entities, cam, 640, 360);
  const r3::FrameStats st = view.end();
  const tr::ViewStats& vs = view.terrain_view().stats();
  if (vs.chunks != 4 || st.foliage == 0 || vs.foliage != st.foliage)
    return fail("null renderer: 4 chunks + foliage (chunks " + std::to_string(vs.chunks) + ", foliage " +
                std::to_string(st.foliage) + ")");
  // Far away the plants fade out.
  cam.position = {-10, 400, 10};
  view.begin(s.environment, s.entities, cam, 640, 360);
  const r3::FrameStats far = view.end();
  if (far.foliage != 0) return fail("foliage fades with distance");
  // Editing the data refreshes the view (shared store).
  std::shared_ptr<tr::TerrainData> d = tr::acquire(dir.string(), *s.entities[0].terrain);
  const std::size_t before = d->foliage.size();
  tr::FoliageBrush eb;
  eb.type = -1;
  eb.radius = 30;
  eb.fill = 1;
  tr::erase_foliage(*d, eb, -10, -10);
  cam.position = {-10, 8, 10};
  view.begin(s.environment, s.entities, cam, 640, 360);
  const r3::FrameStats after = view.end();
  if (!(d->foliage.size() < before) || after.foliage >= st.foliage) return fail("view follows erased foliage");
  std::cout << "[smoke] terrain world OK (hill " << y << " m, Jolt heightfield ray, rider grounded at y "
            << p.y << ", Lua terrain.*, null renderer " << vs.chunks << " chunks / " << st.foliage
            << " plants, fade, live edits)\n";
  tr::forget();
  return 0;
}

// The 3D editor: create a terrain, sculpt / paint / foliage strokes, undo + redo
// restore the heightfield, the data file is saved with the scene.
int editor_checks() {
  namespace ed = ts::tombstone::editor;
  const fs::path dir = fs::absolute("TombStoneProjects/_terrain_editor_smoke");
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  ed::ProjectInfo info;
  info.id = "_terrain_editor_smoke";
  info.name = "Terrain Smoke";
  info.kind = ed::ProjectKind::ThreeD;
  info.path = dir.string();
  ed::Editor3DScreen e(info);
  e.on_enter();
  const std::size_t depth0 = e.undo_depth();
  const std::uint64_t id = e.create_terrain(65, 64.0f, false, "meadow");
  const s3::Entity3D* te = e.scene().find(id);
  if (!id || !te || !te->terrain || te->terrain->foliage.size() != 3 || e.terrain_target() != id)
    return fail("editor: create terrain (meadow: grass, pine, rock)");
  if (!(e.scene().environment.wind_strength > 0.0f)) return fail("editor: a breeze for the foliage");
  const tr::Terrain3D comp = *te->terrain;
  std::shared_ptr<tr::TerrainData> d = tr::acquire(dir.string(), comp);
  const std::vector<float> flat = d->heights;
  auto stroke = [&](int mode, r3::Vec3 at) {
    e.terrain_tool().mode = mode;
    if (!e.terrain_stroke_begin(id)) return false;
    for (int i = 0; i < 10; ++i) e.terrain_stroke(at, 0.05f);
    e.terrain_stroke_end();
    return true;
  };
  const std::size_t depth1 = e.undo_depth();
  e.terrain_tool().op = tr::BrushOp::Raise;
  if (!stroke(0, {0, 0, 0}) || e.undo_depth() != depth1 + 1) return fail("editor: sculpt stroke = one undo step");
  const std::vector<float> raised = d->heights;
  if (raised == flat || !(tr::height_at(*d, 0, 0) > 1.0f)) return fail("editor: raise brush");
  if (!fs::exists(dir / comp.data)) return fail("editor: terrain file saved with the scene");
  e.terrain_tool().foliage_type = 0;
  if (!stroke(2, {10, 0, 10}) || d->foliage.empty()) return fail("editor: foliage stroke");
  e.terrain_tool().layer = 2;
  if (!stroke(1, {-10, 0, -10})) return fail("editor: paint stroke");
  float w[tr::kLayers];
  tr::weights_at(*d, -10, -10, w);
  if (!(w[2] > 0.3f)) return fail("editor: paint layer");
  e.undo();
  e.undo();
  if (!d->foliage.empty()) return fail("editor: undo foliage");
  e.undo();
  if (d->heights != flat) return fail("editor: undo restores the heightfield");
  e.redo();
  if (d->heights != raised) return fail("editor: redo");
  tr::TerrainData disk;
  std::string err;
  if (!tr::load_file((dir / comp.data).string(), &disk, &err) || disk.heights != raised)
    return fail("editor: autosave wrote the redone heightfield");
  e.undo();
  e.undo();
  e.undo();
  if (e.scene().find(id) || e.undo_depth() != depth0) return fail("editor: undo create");
  e.on_exit();
  tr::forget();
  std::cout << "[smoke] terrain editor OK (create meadow terrain + breeze, sculpt / foliage / paint strokes, "
               "undo + redo restore the heightfield, saved with the scene)\n";
  return 0;
}

// ---- Weather ---------------------------------------------------------------------

int sim_checks() {
  wx::Config c;
  c.start = wx::Kind::Clear;
  c.transition = 2.0f;
  c.seed = 5;
  wx::Sim a, b;
  a.configure(c);
  b.configure(c);
  a.set(wx::Kind::Storm);
  b.set(wx::Kind::Storm);
  int strikes = 0;
  for (int i = 0; i < 60 * 40; ++i) {
    a.step(1.0 / 60.0);
    b.step(1.0 / 60.0);
    if (a.state().struck) ++strikes;
  }
  if (a.checksum() != b.checksum()) return fail("Sim deterministic");
  if (a.state().kind != wx::Kind::Storm || !(a.state().mix.rain > 0.5f) || !(a.state().mix.storm > 0.5f))
    return fail("storm settles (rain + storm)");
  if (strikes < 2 || a.state().strikes != static_cast<std::uint64_t>(strikes)) return fail("lightning strikes in a storm");
  a.set(wx::Kind::Snow, 1.0f);
  for (int i = 0; i < 30; ++i) a.step(1.0 / 60.0);
  const float half = a.state().mix.snow;
  for (int i = 0; i < 60; ++i) a.step(1.0 / 60.0);
  if (!(half > 0.1f && half < 0.9f) || !(a.state().mix.snow > 0.9f) || a.state().mix.rain > 0.05f)
    return fail("transition blends to snow");
  a.strike();
  if (!(a.state().flash > 0.5f) && (a.step(0.0), !(a.state().flash > 0.5f))) return fail("strike flashes");
  // Cycle.
  wx::Config cy = c;
  cy.cycle = true;
  cy.steps = {{wx::Kind::Rain, 5}, {wx::Kind::Fog, 5}};
  wx::Sim s;
  s.configure(cy);
  for (int i = 0; i < 60 * 8; ++i) s.step(1.0 / 60.0);
  if (s.state().target != wx::Kind::Fog) return fail("cycle walks to fog");
  // Time of day.
  wx::Config tod = c;
  tod.time_link = true;
  tod.day_length = 100;
  tod.start_time = 0.5f;
  wx::Sim t;
  t.configure(tod);
  t.step(25);
  if (!near(t.state().time_of_day, 0.75f, 0.01f)) return fail("time of day advances");
  if (!(wx::daylight_at(0.5f) > 0.95f) || !(wx::daylight_at(0.0f) < 0.05f)) return fail("daylight");
  // weather.json.
  wx::Config j = cy;
  j.wind_dir = 75;
  j.thunder = false;
  wx::Config back;
  std::string err;
  if (!wx::from_json(wx::to_json(j), &back, &err) || !(back == j)) return fail("weather.json round trip " + err);
  // An unknown kind falls back to clear; broken JSON fails.
  if (!wx::from_json("{\"start\": \"monsoon\"}", &back, &err) || back.start != wx::Kind::Clear)
    return fail("unknown kind -> clear");
  if (wx::from_json("{\"start\": ", &back, &err)) return fail("broken weather.json fails");
  std::cout << "[smoke] weather sim OK (deterministic, storm with " << strikes
            << " strikes, blend, cycle, time of day, weather.json)\n";
  return 0;
}

int plugin2d_checks() {
  wx::Sim s;
  wx::Config c;
  c.start = wx::Kind::Rain;
  s.configure(c);
  for (int i = 0; i < 60; ++i) s.step(1.0 / 60.0);
  runtime::WorldRect view{0, 0, 640, 360};
  std::vector<runtime::DrawQuad> world, hud;
  plugins::WeatherPlugin::draw_world(s.state(), view, &world);
  plugins::WeatherPlugin::draw_hud(s.state(), view, &hud);
  if (world.size() < 50) return fail("2D rain quads (" + std::to_string(world.size()) + ")");
  std::vector<runtime::DrawQuad> again;
  plugins::WeatherPlugin::draw_world(s.state(), view, &again);
  if (again.size() != world.size() || again[0].x0 != world[0].x0) return fail("2D quads are stateless");
  wx::Sim clear;
  std::vector<runtime::DrawQuad> none;
  plugins::WeatherPlugin::draw_world(clear.state(), view, &none);
  if (!none.empty()) return fail("clear sky draws nothing");
  const float scene[3] = {1, 1, 1};
  float amb[3];
  plugins::WeatherPlugin::ambient(s.state(), c, scene, false, amb);
  if (!(amb[0] < 0.95f)) return fail("rain darkens the 2D ambient");
  s.strike();
  s.step(0.0);
  float flash[3];
  plugins::WeatherPlugin::ambient(s.state(), c, scene, false, flash);
  if (!(flash[0] > amb[0])) return fail("lightning brightens the ambient");
  // Particle wind.
  ts::fx::ParticleWorld pw;
  pw.set_wind(3.0f, 0.0f, 0.0f);
  if (!near(pw.wind()[0], 3.0f, 1e-6f)) return fail("particle wind");
  std::cout << "[smoke] weather 2D OK (" << world.size() << " rain quads, " << hud.size()
            << " hud, ambient dims + flashes, particle wind)\n";
  return 0;
}

int weather3d_checks(const fs::path& dir) {
  wx::Config c;
  c.start = wx::Kind::Fog;
  wx::State st;
  wx::Sim s;
  s.configure(c);
  for (int i = 0; i < 600; ++i) s.step(1.0 / 60.0);
  r3::Environment base, env;
  wx::apply_environment(s.state(), c, base, &env);
  if (!(env.fog_density > base.fog_density)) return fail("fog thickens the environment");
  // World with the extension + a script.
  write_file(dir / "weather.json", wx::to_json([] {
               wx::Config k;
               k.start = wx::Kind::Rain;
               k.transition = 0.5f;
               return k;
             }()));
  write_file(dir / "scripts/sky.lua", R"(
function on_start()
  weather.set("snow", 0.5)
end
function on_tick()
  local k = weather.get()
  self:set("kind", k == "snow" and 1 or 0)
  self:set("snow", weather.amount("snow"))
  local wxv, wzv, ws = weather.wind()
  self:set("wind", ws)
end
function on_lightning(_, n) self:set("strikes", n) end
)");
  s3::Scene3D sc;
  s3::Entity3D e;
  e.id = 1;
  e.name = "Sky";
  e.script = ts::tombstone::ScriptData{};
  e.script->path = "scripts/sky.lua";
  sc.entities.push_back(e);
  w3::World3D w;
  auto ext = wx::attach(w, true);
  if (!ext) return fail("attach");
  std::string err;
  if (!w.start(sc, dir.string(), &err)) return fail("start: " + err);
  w.run_ticks(120, {});
  auto num = [&](const char* k) {
    const auto* v = w.state(1, k);
    return v ? v->number : -1.0;
  };
  if (num("kind") != 1 || !(num("snow") > 0.5)) return fail("Lua weather.set / get / amount");
  std::vector<r3::Renderer3D::Billboard> bb;
  w.billboards(&bb);
  if (bb.size() < 50) return fail("3D snow billboards (" + std::to_string(bb.size()) + ")");
  ext->sim().strike();
  w.run_ticks(2, {});
  if (num("strikes") < 1) return fail("on_lightning broadcast");
  if (wx::Weather3D::latest() != ext.get()) return fail("latest()");
  w.stop();
  std::cout << "[smoke] weather 3D OK (fog env, Lua weather.*, " << bb.size()
            << " snow billboards, on_lightning)\n";
  return 0;
}

}  // namespace

int run_terrain_smoke() {
  g_what = "terrain";
  const fs::path dir = temp_project("ts_smoke_terrain");
  if (brushes() || lod_and_files(dir) || foliage_checks() || world_checks(dir) || editor_checks()) return 1;
  return 0;
}

int run_weather_smoke() {
  g_what = "weather";
  const fs::path dir = temp_project("ts_smoke_weather");
  if (sim_checks() || plugin2d_checks() || weather3d_checks(dir)) return 1;
  return 0;
}
