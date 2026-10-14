#include "SmokeFx.h"

#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "core/MemImages.h"
#include "fx/Light2D.h"
#include "fx/Particles.h"
#include "render3d/Scene3D.h"
#include "scene/SceneJson.h"

namespace {

namespace fx = ts::fx;

int g_fail = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::cerr << "[smoke] fx FAILED: " << what << '\n';
    ++g_fail;
  }
}

fx::EmitterDesc round_trip(const fx::EmitterDesc& d) {
  std::map<std::string, std::string> kv;
  for (const fx::Field& f : fx::to_fields(d)) {
    std::string v = f.json;
    if (!v.empty() && v.front() == '"') v = v.substr(1, v.size() - 2);
    kv[f.key] = v;
  }
  return fx::from_fields([&](const std::string& k) -> std::optional<std::string> {
    auto it = kv.find(k);
    if (it == kv.end()) return std::nullopt;
    return it->second;
  });
}

void particles() {
  const float origin[3] = {100, 200, 0};
  for (const std::string& n : fx::preset_names()) {
    for (fx::Space sp : {fx::Space::TwoD, fx::Space::ThreeD}) {
      auto d = fx::preset(n, sp);
      check(d.has_value(), "preset " + n);
      if (!d) continue;
      check(round_trip(*d) == *d, "field round trip " + n);
      fx::ParticleWorld w(sp);
      w.sync(7, *d, origin);
      for (int i = 0; i < 3; ++i) w.update(1.0f / 60.0f);
      check(w.alive() > 0, "preset emits " + n);
    }
  }
  check(!fx::preset("nope", fx::Space::TwoD), "unknown preset refused");
  check(fx::to_fields(fx::EmitterDesc{}).empty(), "defaults write nothing");

  // Burst: exactly burst particles, none more; they all die by life_max.
  fx::EmitterDesc b;
  b.continuous = false;
  b.burst = 40;
  b.life_min = 0.5f;
  b.life_max = 0.5f;
  fx::Emitter e(b, 11);
  e.update(1.0f / 60.0f, origin);
  check(e.alive() == 40, "burst count");
  for (int i = 0; i < 20; ++i) e.update(1.0f / 60.0f, origin);
  check(e.alive() == 40, "burst holds");
  for (int i = 0; i < 20; ++i) e.update(1.0f / 60.0f, origin);
  check(e.alive() == 0 && e.finished(), "burst retires");

  // Continuous: rate 30 for 1 s -> 30 (+-1), capped by max_particles.
  fx::EmitterDesc c;
  c.rate = 30;
  c.life_min = c.life_max = 10;
  fx::Emitter ce(c, 3);
  for (int i = 0; i < 60; ++i) ce.update(1.0f / 60.0f, origin);
  check(std::abs(ce.alive() - 30) <= 1, "continuous rate");
  c.max_particles = 10;
  ce.set_desc(c);
  for (int i = 0; i < 60; ++i) ce.update(1.0f / 60.0f, origin);
  check(ce.alive() <= 30 && ce.alive() >= 10, "cap keeps live ones");
  // Duration stops emission.
  c.max_particles = 256;
  c.duration = 0.5f;
  fx::Emitter de(c, 3);
  for (int i = 0; i < 120; ++i) de.update(1.0f / 60.0f, origin);
  check(std::abs(de.alive() - 15) <= 1, "duration");

  // Physics: gravity + drag; no spread straight up at 100 px/s.
  fx::EmitterDesc g;
  g.continuous = false;
  g.burst = 1;
  g.spread = 0;
  g.speed_min = g.speed_max = 100;
  g.life_min = g.life_max = 5;
  g.gravity[1] = 100;
  fx::Emitter ge(g, 1);
  ge.update(1.0f / 60.0f, origin);  // spawn
  for (int i = 0; i < 60; ++i) ge.update(1.0f / 60.0f, origin);
  std::vector<fx::Sprite> sp;
  ge.sprites(&sp);
  // y: 200 - 100 t + 50 t^2 at t = 1 -> ~150 (+ the spawn tick).
  check(sp.size() == 1 && std::fabs(sp[0].pos[1] - 150.0f) < 4.0f, "gravity");
  g.drag = 5;
  g.gravity[1] = 0;
  fx::Emitter dr(g, 1);
  dr.update(1.0f / 60.0f, origin);
  for (int i = 0; i < 120; ++i) dr.update(1.0f / 60.0f, origin);
  sp.clear();
  dr.sprites(&sp);
  check(sp.size() == 1 && sp[0].pos[1] > 178.0f && sp[0].pos[1] < 182.0f, "drag settles ~20 px");

  // Curves: colour / size keys at start, mid, end.
  fx::EmitterDesc cv;
  cv.continuous = false;
  cv.burst = 1;
  cv.speed_min = cv.speed_max = 0;
  cv.life_min = cv.life_max = 1;
  cv.size[0] = 2;
  cv.size[1] = 10;
  cv.size[2] = 4;
  cv.color_mid[0] = 0;
  cv.color_mid[3] = 1;
  fx::Emitter cve(cv, 5);
  cve.update(1.0f / 60.0f, origin);
  for (int i = 0; i < 29; ++i) cve.update(1.0f / 60.0f, origin);
  sp.clear();
  cve.sprites(&sp);
  check(sp.size() == 1 && std::fabs(sp[0].size / 10.0f - 1.0f) < 0.25f && sp[0].rgba[0] < 0.1f,
        "mid curve key");

  // Local space follows the emitter; world space stays.
  fx::EmitterDesc ls = g;
  ls.drag = 0;
  ls.speed_min = ls.speed_max = 0;
  ls.local_space = true;
  fx::Emitter le(ls, 2), we(g, 2);
  g.speed_min = g.speed_max = 0;
  we = fx::Emitter(g, 2);
  le.update(1.0f / 60.0f, origin);
  we.update(1.0f / 60.0f, origin);
  const float moved[3] = {300, 200, 0};
  le.update(1.0f / 60.0f, moved);
  we.update(1.0f / 60.0f, moved);
  std::vector<fx::Sprite> a, bsp;
  le.sprites(&a);
  we.sprites(&bsp);
  check(a.size() == 1 && bsp.size() == 1 && std::fabs(a[0].pos[0] - 300) < 1 &&
            std::fabs(bsp[0].pos[0] - 100) < 1,
        "local vs world space");

  // Determinism: same seed -> same checksum; another seed differs.
  auto run = [&](std::uint32_t seed) {
    fx::ParticleWorld w;
    fx::EmitterDesc d = *fx::preset("campfire", fx::Space::TwoD);
    d.seed = seed;
    w.sync(1, d, origin);
    w.burst(*fx::preset("sparks", fx::Space::TwoD), origin);
    for (int i = 0; i < 90; ++i) w.update(1.0f / 60.0f);
    return w.checksum();
  };
  check(run(9) == run(9) && run(9) != run(10), "deterministic seed");

  // One-shots retire; stop() lets live ones finish.
  fx::ParticleWorld pw;
  pw.burst(*fx::preset("muzzle_flash", fx::Space::TwoD), origin);
  check(pw.emitters() == 1 && pw.alive() == 8, "one-shot burst");
  for (int i = 0; i < 30; ++i) pw.update(1.0f / 60.0f);
  check(pw.emitters() == 0 && pw.alive() == 0, "one-shot retires");
  pw.sync(3, *fx::preset("campfire", fx::Space::TwoD), origin);
  for (int i = 0; i < 30; ++i) pw.update(1.0f / 60.0f);
  pw.stop(3);
  const int before = pw.alive();
  for (int i = 0; i < 90; ++i) pw.update(1.0f / 60.0f);
  check(before > 0 && pw.alive() == 0, "stop drains");
  pw.prune();
  pw.prune();
  check(!pw.has(3), "prune drops unsynced");

  // Sprite sheet frames over life.
  fx::EmitterDesc sh = cv;
  sh.texture = "assets/fx.png";
  sh.frames_x = 4;
  fx::Emitter she(sh, 1);
  she.update(1.0f / 60.0f, origin);
  for (int i = 0; i < 50; ++i) she.update(1.0f / 60.0f, origin);
  sp.clear();
  she.sprites(&sp);
  check(sp.size() == 1 && sp[0].image && *sp[0].image == "assets/fx.png" &&
            std::fabs(sp[0].u0 - 0.75f) < 1e-4f,
        "sprite sheet frame");
  fx::ensure_builtin_images();
  const auto* dot = ts::tombstone::mem_images::get(fx::kDotImage);
  check(dot && dot->width == 64 && dot->rgba[(32 * 64 + 32) * 4 + 3] > 200 && dot->rgba[3] == 0,
        "soft dot image");
}

void lighting() {
  const float dark[3] = {0.1f, 0.1f, 0.1f};
  fx::Occluders none;
  std::vector<fx::LightInstance> lights(1);
  lights[0].x = 0;
  lights[0].y = 0;
  lights[0].data.radius = 200;
  lights[0].data.color[0] = lights[0].data.color[1] = lights[0].data.color[2] = 1;
  float c[3];
  fx::light_at(0, 0, dark, lights, none, c);
  check(std::fabs(c[0] - 1.1f) < 1e-3f, "light centre");
  fx::light_at(100, 0, dark, lights, none, c);
  check(std::fabs(c[0] - 0.35f) < 1e-3f, "quadratic falloff");
  fx::light_at(250, 0, dark, lights, none, c);
  check(std::fabs(c[0] - 0.1f) < 1e-3f, "outside radius = ambient");
  // Spot: lit along +x, dark behind.
  std::vector<fx::LightInstance> spot = lights;
  spot[0].data.kind = fx::Light2DData::Kind::Spot;
  spot[0].data.cone = 60;
  float front[3], back[3];
  fx::light_at(80, 0, dark, spot, none, front);
  fx::light_at(-80, 0, dark, spot, none, back);
  check(front[0] > 0.4f && back[0] < 0.11f, "spot cone");
  // Shadows: a solid column at cell x=2 (64..96 px) blocks the light.
  fx::Occluders wall;
  wall.cell = 32;
  wall.solid = [](int cx, int cy) { return cx == 2 && cy >= -3 && cy <= 3; };
  lights[0].data.softness = 0;
  lights[0].data.radius = 400;
  float lit[3], shade[3], face[3];
  fx::light_at(150, 10, dark, lights, wall, shade);
  fx::light_at(150, 10, dark, lights, none, lit);
  fx::light_at(70, 10, dark, lights, wall, face);
  check(shade[0] < 0.11f && lit[0] > 0.2f, "tile shadow");
  check(face[0] > 0.3f, "wall face lit");
  lights[0].data.shadows = false;
  fx::light_at(150, 10, dark, lights, wall, shade);
  check(shade[0] > 0.2f, "shadows off");
  // Soft: a point near the shadow edge gets partial light.
  lights[0].data.shadows = true;
  lights[0].data.softness = 1;
  float edge[3];
  fx::light_at(150, 300, dark, lights, wall, edge);
  fx::light_at(150, 300, dark, lights, none, lit);
  check(edge[0] > 0.101f && edge[0] < lit[0] - 0.001f, "soft penumbra");
  // Lightmap: 2x encoding (128 = 1.0), view snapped, ambient fill.
  fx::Lightmap lm;
  fx::LightmapView v{0, 0, 320, 180};
  const float one[3] = {1, 1, 1};
  fx::render_lightmap(v, 8, one, {}, none, 0, &lm);
  check(lm.width >= 40 && lm.height >= 22 && lm.rgba[0] == 128 && lm.rgba[3] == 255,
        "lightmap neutral");
  fx::render_lightmap(v, 8, dark, lights, wall, 0, &lm);
  const int tx = static_cast<int>((0 - lm.view.x0) / 8), ty = static_cast<int>((0 - lm.view.y0) / 8);
  check(lm.rgba[(ty * lm.width + tx) * 4] > 100, "lightmap lit at light");
  // Day-night.
  fx::Lighting2D L;
  L.day_night = true;
  L.start_time = 0.5f;
  L.day_length = 100;
  float amb[3], night = 0;
  L.ambient_at(0, amb, &night);
  check(night == 0 && amb[0] == 1, "noon");
  L.ambient_at(50, amb, &night);
  check(night == 1 && std::fabs(amb[2] - 0.25f) < 1e-4f, "midnight");
}

// Scene formats: 2D v9 particles / light / lighting, 3D v11 particles.
void scenes() {
  namespace sj = ts::tombstone::scene_json;
  namespace s3 = ts::tombstone::scene3d;
  sj::SceneDoc doc;
  ts::tombstone::Entity2D fire;
  fire.id = 1;
  fire.name = "Campfire";
  fire.w = fire.h = 32;
  fire.particles = *fx::preset("campfire", fx::Space::TwoD);
  fire.light = fx::Light2DData{};
  fire.light->radius = 180;
  fire.light->flicker = 0.4f;
  ts::tombstone::Entity2D lamp = fire;
  lamp.id = 2;
  lamp.name = "Lamp";
  lamp.particles.reset();
  lamp.light->kind = fx::Light2DData::Kind::Spot;
  lamp.light->angle = 90;
  lamp.light->cone = 50;
  doc.entities = {fire, lamp};
  doc.lighting.enabled = true;
  doc.lighting.day_night = true;
  doc.lighting.ambient[2] = 0.9f;
  const std::string text = sj::write(doc);
  sj::SceneDoc back;
  std::string err;
  check(sj::parse(text, &back, &err) && back.entities == doc.entities &&
            back.lighting == doc.lighting && sj::write(back) == text,
        "2D v9 round trip: " + err);
  check(text.find("\"version\": 9") != std::string::npos, "2D writes v9");
  // A v8 file (no particles / light / lighting) loads with defaults.
  sj::SceneDoc plain;
  plain.entities = {fire};
  plain.entities[0].particles.reset();
  plain.entities[0].light.reset();
  std::string v8 = sj::write(plain);
  v8.replace(v8.find("\"version\": 9"), 12, "\"version\": 8");
  sj::SceneDoc old;
  check(sj::parse(v8, &old, &err) && old.entities == plain.entities &&
            old.lighting == fx::Lighting2D{},
        "2D v8 loads");
  // 3D v11.
  s3::Scene3D s = s3::make_starter_scene();
  s.entities[0].particles = *fx::preset("sparks", fx::Space::ThreeD);
  s.entities[0].particles->texture = "assets/spark.png";
  s.entities[0].particles->frames_x = 2;
  const std::string t3 = s3::write(s);
  s3::Scene3D b3;
  check(s3::parse(t3, &b3, &err) && b3 == s && s3::write(b3) == t3, "3D v11 round trip: " + err);
  check(t3.find("\"version\": 11") != std::string::npos, "3D writes v11");
}

}  // namespace

int run_fx_smoke() {
  g_fail = 0;
  particles();
  lighting();
  scenes();
  if (g_fail) return 1;
  std::cout << "[smoke] fx OK (10 presets x 2D/3D, field round trip, burst / continuous / "
               "duration / cap, gravity + drag, curves, local space, deterministic seed, "
               "one-shots, sprite sheet, lightmap falloff / spot / tile shadows / soft / "
               "day-night, 2D v9 / 3D v11 scenes)\n";
  return 0;
}
