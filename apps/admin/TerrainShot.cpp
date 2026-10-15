// ts_admin --terrain-shot <out.png>: a hills terrain (grass / dirt / rock /
// sand layers painted by slope and height), pines, oaks, bushes, rocks and
// grass, a breeze and light haze, rendered offscreen by the GL renderer.
#include "core/PngWrite.h"
#include "render3d/GlRenderer3D.h"
#include "render3d/HiddenGl.h"
#include "render3d/Renderer3D.h"
#include "render3d/Scene3D.h"
#include "render3d/SceneView.h"
#include "terrain/Foliage.h"
#include "terrain/Terrain.h"
#include "terrain/TerrainComponent.h"
#include "terrain/TerrainView.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace ts::tombstone;
namespace fs = std::filesystem;

int run_terrain_shot(const std::string& png) {
  namespace r3 = render3d;
  namespace tr = terrain;
  r3::HiddenGl ctx;
  std::string why;
  if (!ctx.open(&why)) {
    std::cerr << "GL context: " << why << '\n';
    return 1;
  }
  auto renderer = r3::make_gl_renderer3d();
  if (!renderer->init(&why)) {
    std::cerr << "GL renderer init: " << why << '\n';
    return 1;
  }
  renderer->set_offscreen(true);
  const int W = 1600, H = 900;

  const fs::path dir = fs::temp_directory_path() / "ts_terrain_shot";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "terrain");

  tr::Terrain3D c;
  c.data = "terrain/valley.tsterrain";
  c.resolution = 257;
  c.size = 192;
  c.lod_distance = 55;
  c.foliage_distance = 1.6f;
  for (const char* n : {"grass", "tall_grass", "pine", "oak", "bush", "rock"}) {
    tr::FoliageType t;
    tr::foliage_preset(n, &t);
    c.foliage.push_back(t);
  }
  c.foliage[0].layer = 0;  // grass only on grass
  c.foliage[1].layer = 0;
  c.foliage[2].density = 0.012f;
  c.foliage[3].density = 0.004f;
  c.foliage[4].density = 0.02f;
  c.foliage[5].density = 0.01f;

  tr::TerrainData t;
  tr::reset(t, c.resolution, c.size);
  tr::generate_hills(t, 4, 16.0f, 52.0f);
  // A dry creek bed: lower a winding line, sand in it.
  tr::Brush b;
  b.op = tr::BrushOp::Lower;
  b.radius = 6;
  b.strength = 3;
  b.hardness = 0.2f;
  for (int i = 0; i <= 80; ++i) {
    const float z = -90.0f + i * 2.25f;
    const float x = 18.0f * std::sin(z * 0.035f) - 10.0f;
    tr::apply_brush(t, b, x, z, 0.12f);
    tr::Brush s = b;
    s.op = tr::BrushOp::Paint;
    s.layer = 3;
    s.strength = 30;
    s.radius = 4.5f;
    tr::apply_brush(t, s, x, z, 0.3f);
  }
  // Rock on the steep faces, dirt on the shoulders.
  for (int j = 0; j < t.res; ++j) {
    for (int i = 0; i < t.res; ++i) {
      const tr::Vec3 n = tr::normal_at(t, t.wx(i), t.wz(j));
      std::uint8_t* w = &t.splat[(static_cast<std::size_t>(j) * t.res + i) * 4];
      if (w[3] > 128) continue;
      float rock = std::clamp((0.93f - n.y) * 9.0f, 0.0f, 1.0f);
      float dirt = std::clamp((0.975f - n.y) * 14.0f, 0.0f, 1.0f) * (1.0f - rock);
      dirt = std::max(dirt, tr::hash_unit(i / 6, j / 6, 9) > 0.86f ? 0.6f : 0.0f) * (1.0f - rock);
      const float grass = std::max(0.0f, 1.0f - rock - dirt);
      w[0] = static_cast<std::uint8_t>(grass * 255.0f);
      w[1] = static_cast<std::uint8_t>(dirt * 255.0f);
      w[2] = static_cast<std::uint8_t>(rock * 255.0f);
      w[3] = static_cast<std::uint8_t>(255 - w[0] - w[1] - w[2]);
    }
  }
  tr::note_edit(t, tr::full_rect(t), true, true, false);
  // Plants everywhere their rules allow.
  for (int type = 0; type < static_cast<int>(c.foliage.size()); ++type) {
    tr::FoliageBrush fb;
    fb.type = type;
    fb.radius = c.size;
    fb.fill = type <= 1 ? (type == 0 ? 0.55f : 0.25f) : 1.0f;
    fb.seed = 31 + type;
    tr::paint_foliage(t, c.foliage, fb, 0, 0);
  }
  std::string err;
  if (!tr::save_file((dir / c.data).string(), t, &err)) {
    std::cerr << err << '\n';
    return 1;
  }

  scene3d::Scene3D s;
  scene3d::Entity3D e;
  e.id = 1;
  e.name = "Valley";
  e.terrain = c;
  s.entities.push_back(e);
  r3::Environment& env = s.environment;
  env.sky_top[0] = 0.30f; env.sky_top[1] = 0.45f; env.sky_top[2] = 0.70f;
  env.sky_horizon[0] = 0.86f; env.sky_horizon[1] = 0.80f; env.sky_horizon[2] = 0.70f;
  env.ground[0] = 0.30f; env.ground[1] = 0.28f; env.ground[2] = 0.20f;
  env.sun_direction = r3::normalize(r3::Vec3{-0.55f, -0.5f, 0.45f});
  env.sun_intensity = 2.8f;
  env.ambient = 0.42f;
  env.shadow_distance = 60.0f;
  env.fog_color[0] = 0.80f; env.fog_color[1] = 0.80f; env.fog_color[2] = 0.76f;
  env.fog_density = 0.008f;
  env.fog_height = 6.0f;
  env.fog_falloff = 0.05f;
  env.wind_strength = 0.35f;
  env.wind_gust = 0.3f;

  r3::Camera3D cam;
  const float gx = 30.0f, gz = 70.0f;
  cam.position = {gx, tr::height_at(t, gx, gz) + 7.0f, gz};
  cam.target = {-15.0f, tr::height_at(t, -15.0f, -10.0f) + 1.0f, -10.0f};
  cam.fov_y_degrees = 60.0f;
  cam.z_far = 600.0f;

  r3::SceneView view(renderer.get());
  view.set_project_dir(dir.string());
  r3::FrameStats st;
  for (int frame = 0; frame < 3; ++frame) {
    view.set_time(2.0 + frame * 0.016);
    view.begin(env, s.entities, cam, W, H);
    st = view.end();
  }
  std::vector<unsigned char> px;
  int w = 0, h = 0;
  if (!renderer->read_pixels(&px, &w, &h) || w != W || h != H) {
    std::cerr << "read_pixels failed\n";
    return 1;
  }
  std::vector<unsigned char> flipped(px.size());
  for (int y = 0; y < h; ++y)
    std::memcpy(&flipped[static_cast<std::size_t>(y) * w * 4], &px[static_cast<std::size_t>(h - 1 - y) * w * 4],
                static_cast<std::size_t>(w) * 4);
  for (std::size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;
  if (!write_png_rgba(png, w, h, flipped)) {
    std::cerr << "cannot write " << png << '\n';
    return 1;
  }
  const tr::ViewStats& vs = view.terrain_view().stats();
  std::cout << "Terrain screenshot: " << png << " (" << w << "x" << h << ", " << vs.chunks << " chunks, LOD "
            << vs.lod_counts[0] << "/" << vs.lod_counts[1] << "/" << vs.lod_counts[2] << "/" << vs.lod_counts[3]
            << ", " << st.foliage << " plants of " << t.foliage.size() << ", " << st.draws << " draws)\n";
  return 0;
}
