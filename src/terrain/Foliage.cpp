#include "terrain/Foliage.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace ts {
namespace tombstone {
namespace terrain {

using render3d::MeshData;
using render3d::Vertex3D;

void normalize(FoliageType& t) {
  t.density = std::clamp(t.density, 0.001f, 64.0f);
  t.scale_min = std::clamp(t.scale_min, 0.05f, 20.0f);
  t.scale_max = std::clamp(t.scale_max, t.scale_min, 20.0f);
  t.sway = std::clamp(t.sway, 0.0f, 4.0f);
  t.fade_end = std::clamp(t.fade_end, 2.0f, 5000.0f);
  t.fade_start = std::clamp(t.fade_start, 0.0f, t.fade_end);
  t.slope_max = std::clamp(t.slope_max, 0.0f, 90.0f);
  t.layer = std::clamp(t.layer, -1, kLayers - 1);
  for (float& c : t.color_a) c = std::clamp(c, 0.0f, 1.0f);
  for (float& c : t.color_b) c = std::clamp(c, 0.0f, 1.0f);
  t.collide_radius = std::clamp(t.collide_radius, 0.05f, 10.0f);
  t.collide_height = std::clamp(t.collide_height, 0.1f, 100.0f);
  if (t.mesh.empty()) t.mesh = "foliage:grass";
}

const std::vector<std::string>& foliage_preset_names() {
  static const std::vector<std::string> n = {"grass", "tall_grass", "pine", "oak", "bush", "rock", "cactus"};
  return n;
}

bool foliage_preset(const std::string& name, FoliageType* out) {
  FoliageType t;
  t.name = name;
  auto col = [](float* d, float r, float g, float b) { d[0] = r; d[1] = g; d[2] = b; };
  if (name == "grass") {
    // defaults
  } else if (name == "tall_grass") {
    t.mesh = "foliage:tall_grass";
    t.density = 1.5f;
    col(t.color_a, 0.35f, 0.33f, 0.14f);
    col(t.color_b, 0.80f, 0.72f, 0.40f);
    t.fade_start = 45;
    t.fade_end = 65;
  } else if (name == "pine") {
    t.mesh = "foliage:pine";
    t.density = 0.02f;
    t.scale_min = 0.8f;
    t.scale_max = 1.5f;
    t.sway = 0.35f;
    t.fade_start = 220;
    t.fade_end = 300;
    col(t.color_a, 0.30f, 0.20f, 0.12f);
    col(t.color_b, 0.13f, 0.30f, 0.15f);
    t.slope_max = 35;
    t.cast_shadows = true;
    t.collide = true;
    t.collide_radius = 0.25f;
    t.collide_height = 4.0f;
  } else if (name == "oak") {
    t.mesh = "foliage:oak";
    t.density = 0.015f;
    t.scale_min = 0.9f;
    t.scale_max = 1.4f;
    t.sway = 0.3f;
    t.fade_start = 220;
    t.fade_end = 300;
    col(t.color_a, 0.33f, 0.24f, 0.16f);
    col(t.color_b, 0.27f, 0.42f, 0.16f);
    t.slope_max = 30;
    t.cast_shadows = true;
    t.collide = true;
    t.collide_radius = 0.3f;
  } else if (name == "bush") {
    t.mesh = "foliage:bush";
    t.density = 0.15f;
    t.sway = 0.4f;
    t.fade_start = 70;
    t.fade_end = 95;
    col(t.color_a, 0.20f, 0.30f, 0.12f);
    col(t.color_b, 0.36f, 0.46f, 0.20f);
    t.cast_shadows = true;
  } else if (name == "rock") {
    t.mesh = "foliage:rock";
    t.density = 0.05f;
    t.scale_min = 0.4f;
    t.scale_max = 1.8f;
    t.sway = 0.0f;
    t.fade_start = 120;
    t.fade_end = 160;
    col(t.color_a, 0.36f, 0.33f, 0.30f);
    col(t.color_b, 0.55f, 0.52f, 0.47f);
    t.slope_max = 60;
    t.cast_shadows = true;
  } else if (name == "cactus") {
    t.mesh = "foliage:cactus";
    t.density = 0.01f;
    t.scale_min = 0.7f;
    t.scale_max = 1.3f;
    t.sway = 0.05f;
    t.fade_start = 180;
    t.fade_end = 240;
    col(t.color_a, 0.20f, 0.34f, 0.18f);
    col(t.color_b, 0.36f, 0.50f, 0.28f);
    t.cast_shadows = true;
    t.collide = true;
    t.collide_radius = 0.3f;
    t.collide_height = 3.0f;
  } else {
    return false;
  }
  *out = t;
  return true;
}

// Keep in step with ts_wind_bend / ts_sway in GlTerrainFoliage.cpp.
float wind_bend(float x, float z, float time, const Wind& w) {
  const float ph = time * (1.3f + 0.7f * w.strength) + x * 0.13f + z * 0.11f;
  return w.strength * (0.6f + 0.4f * std::sin(ph)) + w.gust * 0.5f * std::sin(ph * 2.7f + x * 0.3f);
}

Vec3 sway_offset(float bend, float weight, float sway, float scale, const Wind& w) {
  const float k = bend * sway * weight * weight * 0.35f * scale;
  return {w.dir_x * k, -std::fabs(k) * 0.15f, w.dir_z * k};
}

namespace {
std::int64_t cell_key(int a, int b) {
  return (static_cast<std::int64_t>(a) << 32) ^ static_cast<std::uint32_t>(b);
}
}  // namespace

int paint_foliage(TerrainData& t, const std::vector<FoliageType>& types, const FoliageBrush& b,
                  float x, float z) {
  if (!t.valid() || b.type < 0 || b.type >= static_cast<int>(types.size()) || b.radius <= 0) return 0;
  FoliageType ft = types[static_cast<std::size_t>(b.type)];
  normalize(ft);
  const float cell = 1.0f / std::sqrt(ft.density);
  const int c0 = static_cast<int>(std::floor((x - b.radius) / cell));
  const int c1 = static_cast<int>(std::floor((x + b.radius) / cell));
  const int d0 = static_cast<int>(std::floor((z - b.radius) / cell));
  const int d1 = static_cast<int>(std::floor((z + b.radius) / cell));
  if (static_cast<long long>(c1 - c0 + 1) * (d1 - d0 + 1) > 400000) return 0;  // brush too big for density
  std::unordered_set<std::int64_t> taken;
  for (const FoliageInstance& f : t.foliage) {
    if (f.type != b.type) continue;
    if (std::fabs(f.x - x) > b.radius + cell || std::fabs(f.z - z) > b.radius + cell) continue;
    taken.insert(cell_key(static_cast<int>(std::floor(f.x / cell)), static_cast<int>(std::floor(f.z / cell))));
  }
  const float cos_max = std::cos(render3d::radians(ft.slope_max));
  const std::uint32_t tseed = b.seed * 31u + static_cast<std::uint32_t>(b.type) * 7919u;
  int added = 0;
  for (int cz = d0; cz <= d1; ++cz) {
    for (int cx = c0; cx <= c1; ++cx) {
      if (taken.count(cell_key(cx, cz))) continue;
      const auto ux = static_cast<std::uint32_t>(cx), uz = static_cast<std::uint32_t>(cz);
      if (hash_unit(ux, uz, tseed) >= b.fill) continue;
      const float px = (static_cast<float>(cx) + hash_unit(ux, uz, tseed + 1)) * cell;
      const float pz = (static_cast<float>(cz) + hash_unit(ux, uz, tseed + 2)) * cell;
      if (std::hypot(px - x, pz - z) > b.radius || !inside(t, px, pz)) continue;
      if (normal_at(t, px, pz).y < cos_max) continue;
      if (ft.layer >= 0) {
        float w[kLayers];
        weights_at(t, px, pz, w);
        if (w[ft.layer] < 0.5f) continue;
      }
      FoliageInstance f;
      f.type = static_cast<std::uint16_t>(b.type);
      f.x = px;
      f.z = pz;
      f.y = height_at(t, px, pz);
      f.yaw = hash_unit(ux, uz, tseed + 3) * 2.0f * render3d::kPi;
      f.scale = ft.scale_min + (ft.scale_max - ft.scale_min) * hash_unit(ux, uz, tseed + 4);
      t.foliage.push_back(f);
      ++added;
    }
  }
  if (added) note_edit(t, Rect{}, false, false, true);
  return added;
}

int erase_foliage(TerrainData& t, const FoliageBrush& b, float x, float z) {
  const std::size_t before = t.foliage.size();
  const float r2 = b.radius * b.radius;
  std::erase_if(t.foliage, [&](const FoliageInstance& f) {
    if (b.type >= 0 && f.type != b.type) return false;
    const float dx = f.x - x, dz = f.z - z;
    if (dx * dx + dz * dz > r2) return false;
    if (b.fill >= 1.0f) return true;
    std::uint32_t bx, bz;
    std::memcpy(&bx, &f.x, 4);
    std::memcpy(&bz, &f.z, 4);
    return hash_unit(bx, bz, b.seed) < b.fill;
  });
  const int removed = static_cast<int>(before - t.foliage.size());
  if (removed) note_edit(t, Rect{}, false, false, true);
  return removed;
}

void remove_foliage_type(TerrainData& t, int type) {
  std::erase_if(t.foliage, [&](const FoliageInstance& f) { return f.type == type; });
  for (FoliageInstance& f : t.foliage)
    if (f.type > type) --f.type;
  note_edit(t, Rect{}, false, false, true);
}

int count_foliage(const TerrainData& t, int type) {
  int n = 0;
  for (const FoliageInstance& f : t.foliage)
    if (type < 0 || f.type == type) ++n;
  return n;
}

void FoliageIndex::rebuild(const TerrainData& t) {
  revision_ = t.revision;
  count_ = t.foliage.size();
  n_ = std::max(1, static_cast<int>(std::ceil(t.size / kCell)));
  cells_.assign(static_cast<std::size_t>(n_) * n_, {});
  const float half = t.size * 0.5f;
  for (std::size_t i = 0; i < t.foliage.size(); ++i) {
    const FoliageInstance& f = t.foliage[i];
    const int cx = std::clamp(static_cast<int>((f.x + half) / kCell), 0, n_ - 1);
    const int cz = std::clamp(static_cast<int>((f.z + half) / kCell), 0, n_ - 1);
    cells_[static_cast<std::size_t>(cz) * n_ + cx].push_back(static_cast<std::uint32_t>(i));
  }
}

void FoliageIndex::gather(const TerrainData& t, const std::vector<FoliageType>& types, const Vec3& origin,
                          const Vec3& cam, float distance_scale,
                          std::vector<std::vector<FoliageGpuInstance>>* out) {
  out->resize(types.size());
  for (auto& v : *out) v.clear();
  visited_ = 0;
  if (!t.valid() || types.empty()) return;
  if (revision_ != t.revision || count_ != t.foliage.size()) rebuild(t);
  float reach = 0;
  for (const FoliageType& ft : types) reach = std::max(reach, ft.fade_end * distance_scale);
  const float half = t.size * 0.5f;
  const Vec3 c = cam - origin;
  const int c0 = std::clamp(static_cast<int>((c.x - reach + half) / kCell), 0, n_ - 1);
  const int c1 = std::clamp(static_cast<int>((c.x + reach + half) / kCell), 0, n_ - 1);
  const int d0 = std::clamp(static_cast<int>((c.z - reach + half) / kCell), 0, n_ - 1);
  const int d1 = std::clamp(static_cast<int>((c.z + reach + half) / kCell), 0, n_ - 1);
  for (int cz = d0; cz <= d1; ++cz) {
    for (int cx = c0; cx <= c1; ++cx) {
      for (std::uint32_t idx : cells_[static_cast<std::size_t>(cz) * n_ + cx]) {
        const FoliageInstance& f = t.foliage[idx];
        if (f.type >= types.size()) continue;
        ++visited_;
        const FoliageType& ft = types[f.type];
        const float dx = f.x - c.x, dy = f.y - c.y, dz = f.z - c.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float fs = ft.fade_start * distance_scale, fe = ft.fade_end * distance_scale;
        if (d >= fe) continue;
        float fade = 1.0f;
        if (d > fs) {
          const float k = (d - fs) / std::max(0.001f, fe - fs);
          fade = 1.0f - k * k * (3 - 2 * k);
        }
        (*out)[f.type].push_back({f.x + origin.x, f.y + origin.y, f.z + origin.z, f.yaw, f.scale, fade});
      }
    }
  }
}

// ---- Built-in meshes -----------------------------------------------------

namespace {

struct Builder {
  MeshData m;
  float height = 1.0f;     // sway weight = y / height
  float sway_scale = 1.0f;
  bool soft_normals = false;  // grass: bias normals up (reads better lit)

  void tri(Vec3 a, Vec3 b, Vec3 c, float pa, float pb, float pc) {
    Vec3 n = render3d::normalize(render3d::cross(b - a, c - a));
    if (soft_normals) n = render3d::normalize(n * 0.35f + Vec3{0, 1, 0});
    const std::uint32_t base = static_cast<std::uint32_t>(m.vertices.size());
    const Vec3 p[3] = {a, b, c};
    const float part[3] = {pa, pb, pc};
    for (int i = 0; i < 3; ++i) {
      Vertex3D v;
      v.position = p[i];
      v.normal = n;
      v.u = std::clamp(part[i], 0.0f, 1.0f);
      v.v = std::clamp(p[i].y / height, 0.0f, 1.0f) * sway_scale;
      m.vertices.push_back(v);
      m.indices.push_back(base + static_cast<std::uint32_t>(i));
    }
  }
  void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, float pa, float pb, float pc, float pd) {
    tri(a, b, c, pa, pb, pc);
    tri(a, c, d, pa, pc, pd);
  }
  // Prism / frustum along y with `sides` faces.
  void column(Vec3 base, float r0, float r1, float h, int sides, float part0, float part1, bool cap) {
    for (int s = 0; s < sides; ++s) {
      const float a0 = 2 * render3d::kPi * s / sides, a1 = 2 * render3d::kPi * (s + 1) / sides;
      const Vec3 b0 = base + Vec3{std::cos(a0) * r0, 0, std::sin(a0) * r0};
      const Vec3 b1 = base + Vec3{std::cos(a1) * r0, 0, std::sin(a1) * r0};
      const Vec3 t0 = base + Vec3{std::cos(a0) * r1, h, std::sin(a0) * r1};
      const Vec3 t1 = base + Vec3{std::cos(a1) * r1, h, std::sin(a1) * r1};
      if (r1 > 0.0001f) quad(b0, t0, t1, b1, part0, part1, part1, part0);
      else tri(b0, t0, b1, part0, part1, part0);
      if (cap && r1 > 0.0001f) tri(base + Vec3{0, h, 0}, t1, t0, part1, part1, part1);
    }
  }
  // Faceted blob (lat-long sphere) with optional noise.
  void blob(Vec3 c, Vec3 r, int seg, int rings, float part_lo, float part_hi, float noise,
            std::uint32_t seed) {
    auto pt = [&](int s, int k) {
      const float th = render3d::kPi * k / rings;
      const float ph = 2 * render3d::kPi * (s % seg) / seg;
      float rr = 1.0f;
      if (noise > 0 && k > 0 && k < rings)
        rr += (hash_unit(static_cast<std::uint32_t>(s % seg), static_cast<std::uint32_t>(k), seed) - 0.5f) * noise;
      return c + Vec3{std::sin(th) * std::cos(ph) * r.x * rr, std::cos(th) * r.y * rr, std::sin(th) * std::sin(ph) * r.z * rr};
    };
    auto part = [&](const Vec3& p) {
      const float k = std::clamp((p.y - (c.y - r.y)) / (2 * r.y), 0.0f, 1.0f);
      return part_lo + (part_hi - part_lo) * k;
    };
    for (int k = 0; k < rings; ++k) {
      for (int s = 0; s < seg; ++s) {
        const Vec3 a = pt(s, k), b = pt(s + 1, k), cc = pt(s + 1, k + 1), d = pt(s, k + 1);
        if (k == 0) tri(a, cc, d, part(a), part(cc), part(d));
        else if (k == rings - 1) tri(a, b, d, part(a), part(b), part(d));
        else quad(a, b, cc, d, part(a), part(b), part(cc), part(d));
      }
    }
  }
};

void grass(Builder& b, float h, int blades, std::uint32_t seed) {
  b.height = h;
  b.soft_normals = true;
  for (int i = 0; i < blades; ++i) {
    const float a = 2 * render3d::kPi * (i + hash_unit(i, 1, seed) * 0.6f) / blades;
    const float r = 0.04f + hash_unit(i, 2, seed) * 0.08f;
    const float bh = h * (0.7f + hash_unit(i, 3, seed) * 0.3f);
    const float w = 0.035f * (h / 0.5f > 1.5f ? 1.4f : 1.0f);
    const Vec3 at{std::cos(a) * r, 0, std::sin(a) * r};
    const Vec3 side{-std::sin(a) * w, 0, std::cos(a) * w};
    const Vec3 lean = Vec3{std::cos(a), 0, std::sin(a)} * (bh * 0.25f);
    const Vec3 m = at + Vec3{0, bh * 0.55f, 0} + lean * 0.35f;
    const Vec3 tip = at + Vec3{0, bh, 0} + lean;
    b.quad(at - side, at + side, m + side * 0.7f, m - side * 0.7f, 0, 0, 0.55f, 0.55f);
    b.tri(m - side * 0.7f, m + side * 0.7f, tip, 0.55f, 0.55f, 1.0f);
  }
}

}  // namespace

const std::vector<std::string>& foliage_mesh_names() {
  static const std::vector<std::string> n = {"foliage:grass", "foliage:tall_grass", "foliage:pine",
                                             "foliage:oak",   "foliage:bush",       "foliage:rock",
                                             "foliage:cactus"};
  return n;
}

bool is_builtin_foliage_mesh(const std::string& name) {
  const auto& n = foliage_mesh_names();
  return std::find(n.begin(), n.end(), name) != n.end();
}

bool make_foliage_mesh(const std::string& name, MeshData* out) {
  Builder b;
  if (name == "foliage:grass") {
    grass(b, 0.5f, 7, 11);
  } else if (name == "foliage:tall_grass") {
    grass(b, 1.1f, 9, 23);
  } else if (name == "foliage:pine") {
    b.height = 6.0f;
    b.column({0, 0, 0}, 0.18f, 0.12f, 1.6f, 6, 0, 0, false);
    const float ys[3] = {1.1f, 2.4f, 3.6f}, rs[3] = {1.6f, 1.25f, 0.85f}, hs[3] = {2.2f, 1.9f, 1.8f};
    for (int i = 0; i < 3; ++i) {
      b.column({0, ys[i], 0}, rs[i], 0, hs[i], 9, 0.75f, 1.0f, false);
      // Underside.
      for (int s = 0; s < 9; ++s) {
        const float a0 = 2 * render3d::kPi * s / 9, a1 = 2 * render3d::kPi * (s + 1) / 9;
        b.tri({0, ys[i] + 0.3f, 0}, {std::cos(a0) * rs[i], ys[i], std::sin(a0) * rs[i]},
              {std::cos(a1) * rs[i], ys[i], std::sin(a1) * rs[i]}, 0.6f, 0.7f, 0.7f);
      }
    }
  } else if (name == "foliage:oak") {
    b.height = 5.5f;
    b.column({0, 0, 0}, 0.24f, 0.16f, 2.6f, 7, 0, 0.1f, false);
    b.column({0, 2.0f, 0}, 0.1f, 0.05f, 1.2f, 5, 0, 0.1f, false);
    b.blob({0, 3.6f, 0}, {1.7f, 1.3f, 1.7f}, 9, 6, 0.65f, 1.0f, 0.25f, 3);
    b.blob({0.9f, 3.1f, 0.4f}, {1.1f, 0.9f, 1.1f}, 8, 5, 0.6f, 0.95f, 0.25f, 5);
    b.blob({-0.8f, 3.2f, -0.5f}, {1.0f, 0.85f, 1.0f}, 8, 5, 0.6f, 0.95f, 0.25f, 9);
  } else if (name == "foliage:bush") {
    b.height = 1.2f;
    b.sway_scale = 0.8f;
    b.blob({0, 0.45f, 0}, {0.75f, 0.55f, 0.75f}, 8, 5, 0.2f, 1.0f, 0.3f, 2);
    b.blob({0.45f, 0.35f, 0.2f}, {0.5f, 0.4f, 0.5f}, 7, 4, 0.2f, 0.9f, 0.3f, 4);
    b.blob({-0.35f, 0.3f, -0.3f}, {0.45f, 0.38f, 0.45f}, 7, 4, 0.2f, 0.9f, 0.3f, 6);
  } else if (name == "foliage:rock") {
    b.height = 1.0f;
    b.sway_scale = 0.0f;
    b.blob({0, 0.25f, 0}, {0.8f, 0.55f, 0.65f}, 9, 6, 0.0f, 1.0f, 0.45f, 17);
  } else if (name == "foliage:cactus") {
    b.height = 3.2f;
    b.sway_scale = 0.3f;
    b.column({0, 0, 0}, 0.26f, 0.24f, 2.8f, 8, 0.2f, 0.9f, false);
    b.blob({0, 2.8f, 0}, {0.24f, 0.2f, 0.24f}, 8, 4, 0.9f, 1.0f, 0, 1);
    // Arms: elbow out then up.
    for (int side : {-1, 1}) {
      const float y = side < 0 ? 1.2f : 1.6f;
      const float sx = static_cast<float>(side);
      for (int s = 0; s < 6; ++s) {
        const float a0 = 2 * render3d::kPi * s / 6, a1 = 2 * render3d::kPi * (s + 1) / 6;
        const Vec3 p0{0.1f * sx, y + std::cos(a0) * 0.15f, std::sin(a0) * 0.15f};
        const Vec3 p1{0.1f * sx, y + std::cos(a1) * 0.15f, std::sin(a1) * 0.15f};
        const Vec3 q0 = p0 + Vec3{0.55f * sx, 0, 0}, q1 = p1 + Vec3{0.55f * sx, 0, 0};
        if (side > 0) b.quad(p0, q0, q1, p1, 0.5f, 0.6f, 0.6f, 0.5f);
        else b.quad(p0, p1, q1, q0, 0.5f, 0.5f, 0.6f, 0.6f);
      }
      b.column({0.65f * sx, y - 0.1f, 0}, 0.16f, 0.15f, 0.9f, 6, 0.6f, 0.95f, true);
    }
  } else {
    return false;
  }
  b.m.name = name;
  *out = std::move(b.m);
  return true;
}

}  // namespace terrain
}  // namespace tombstone
}  // namespace ts
