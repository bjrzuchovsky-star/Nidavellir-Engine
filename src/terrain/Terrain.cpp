#include "terrain/Terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace terrain {

using render3d::Aabb;
using render3d::Image;
using render3d::MeshData;
using render3d::Vertex3D;

std::uint32_t hash3(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
  std::uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return h;
}

float hash_unit(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
  return static_cast<float>(hash3(a, b, c) >> 8) / 16777216.0f;
}

float value_noise(float x, float z, std::uint32_t seed) {
  const float fx = std::floor(x), fz = std::floor(z);
  const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
  float tx = x - fx, tz = z - fz;
  tx = tx * tx * (3 - 2 * tx);
  tz = tz * tz * (3 - 2 * tz);
  auto v = [&](int a, int b) {
    return hash_unit(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b), seed);
  };
  const float a = v(ix, iz) + (v(ix + 1, iz) - v(ix, iz)) * tx;
  const float b = v(ix, iz + 1) + (v(ix + 1, iz + 1) - v(ix, iz + 1)) * tx;
  return a + (b - a) * tz;
}

void note_edit(TerrainData& t, const Rect& r, bool heights, bool splat, bool foliage) {
  ++t.revision;
  Edit e;
  e.revision = t.revision;
  e.rect = r;
  e.heights = heights;
  e.splat = splat;
  e.foliage = foliage;
  t.edits.push_back(e);
  if (t.edits.size() > 256) t.edits.erase(t.edits.begin(), t.edits.begin() + 128);
}

Rect full_rect(const TerrainData& t) { return Rect{0, 0, t.res - 1, t.res - 1}; }

bool edits_since(const TerrainData& t, std::uint64_t since, Rect* heights, bool* splat, bool* foliage) {
  *heights = Rect{};
  *splat = false;
  *foliage = false;
  if (since == t.revision) return true;
  if (since > t.revision) return false;
  if (t.edits.empty() || t.edits.front().revision > since + 1) return false;
  for (const Edit& e : t.edits) {
    if (e.revision <= since) continue;
    if (e.heights) heights->merge(e.rect);
    *splat = *splat || e.splat;
    *foliage = *foliage || e.foliage;
  }
  return true;
}

bool valid_resolution(int res) {
  if (res < 17 || res > 1025) return false;
  const int q = res - 1;
  return (q & (q - 1)) == 0;
}

void reset(TerrainData& t, int res, float size) {
  if (!valid_resolution(res)) res = 129;
  t.res = res;
  t.size = std::clamp(size, 4.0f, 8192.0f);
  t.heights.assign(static_cast<std::size_t>(res) * res, 0.0f);
  t.splat.assign(t.heights.size() * 4, 0);
  for (std::size_t i = 0; i < t.heights.size(); ++i) t.splat[i * 4] = 255;
  t.foliage.clear();
  t.edits.clear();
  ++t.revision;
}

void generate_hills(TerrainData& t, std::uint32_t seed, float amplitude, float feature) {
  if (!t.valid()) return;
  feature = std::max(1.0f, feature);
  for (int j = 0; j < t.res; ++j) {
    for (int i = 0; i < t.res; ++i) {
      const float x = t.wx(i) / feature, z = t.wz(j) / feature;
      float sum = 0, amp = 1, norm = 0, f = 1;
      for (int o = 0; o < 5; ++o) {
        sum += value_noise(x * f, z * f, seed + static_cast<std::uint32_t>(o) * 101u) * amp;
        norm += amp;
        amp *= 0.5f;
        f *= 2.0f;
      }
      t.h(i, j) = (sum / norm - 0.5f) * 2.0f * amplitude;
    }
  }
  refit_foliage(t, full_rect(t));
  note_edit(t, full_rect(t), true, false, true);
}

bool inside(const TerrainData& t, float x, float z) {
  const float h = t.size * 0.5f;
  return x >= -h && x <= h && z >= -h && z <= h;
}

float height_at(const TerrainData& t, float x, float z) {
  if (!t.valid()) return 0.0f;
  const float max = static_cast<float>(t.res - 1);
  const float gx = std::clamp(t.gx(x), 0.0f, max), gz = std::clamp(t.gz(z), 0.0f, max);
  const int i = std::min(static_cast<int>(gx), t.res - 2), j = std::min(static_cast<int>(gz), t.res - 2);
  const float fx = gx - static_cast<float>(i), fz = gz - static_cast<float>(j);
  const float a = t.h(i, j) + (t.h(i + 1, j) - t.h(i, j)) * fx;
  const float b = t.h(i, j + 1) + (t.h(i + 1, j + 1) - t.h(i, j + 1)) * fx;
  return a + (b - a) * fz;
}

Vec3 normal_at(const TerrainData& t, float x, float z) {
  const float e = t.valid() ? t.cell() : 1.0f;
  const float dx = height_at(t, x + e, z) - height_at(t, x - e, z);
  const float dz = height_at(t, x, z + e) - height_at(t, x, z - e);
  return render3d::normalize(Vec3{-dx, 2.0f * e, -dz});
}

namespace {
Vec3 grid_normal(const TerrainData& t, int i, int j) {
  const int i0 = std::max(0, i - 1), i1 = std::min(t.res - 1, i + 1);
  const int j0 = std::max(0, j - 1), j1 = std::min(t.res - 1, j + 1);
  const float c = t.cell();
  const float dx = (t.h(i1, j) - t.h(i0, j)) / (static_cast<float>(i1 - i0) * c);
  const float dz = (t.h(i, j1) - t.h(i, j0)) / (static_cast<float>(j1 - j0) * c);
  return render3d::normalize(Vec3{-dx, 1.0f, -dz});
}
}  // namespace

void weights_at(const TerrainData& t, float x, float z, float out[kLayers]) {
  for (int k = 0; k < kLayers; ++k) out[k] = 0;
  if (!t.valid()) return;
  const int i = std::clamp(static_cast<int>(std::lround(t.gx(x))), 0, t.res - 1);
  const int j = std::clamp(static_cast<int>(std::lround(t.gz(z))), 0, t.res - 1);
  const std::size_t o = (static_cast<std::size_t>(j) * t.res + i) * 4;
  float sum = 0;
  for (int k = 0; k < kLayers; ++k) sum += t.splat[o + k];
  for (int k = 0; k < kLayers; ++k) out[k] = sum > 0 ? t.splat[o + k] / sum : (k == 0 ? 1.0f : 0.0f);
}

bool raycast(const TerrainData& t, const Vec3& o, const Vec3& d, float max_dist, float* t_out) {
  if (!t.valid()) return false;
  const float step = t.cell() * 0.5f;
  auto above = [&](float s) {
    const Vec3 p = o + d * s;
    return p.y - height_at(t, p.x, p.z);
  };
  float prev_s = 0, prev = above(0);
  if (prev < 0 && inside(t, o.x, o.z)) {
    if (t_out) *t_out = 0;
    return true;
  }
  for (float s = step; s <= max_dist; s += step) {
    const Vec3 p = o + d * s;
    const float cur = above(s);
    if (cur <= 0 && inside(t, p.x, p.z)) {
      float a = prev_s, b = s;
      for (int k = 0; k < 20; ++k) {
        const float m = (a + b) * 0.5f;
        if (above(m) > 0) a = m; else b = m;
      }
      if (t_out) *t_out = (a + b) * 0.5f;
      return true;
    }
    prev_s = s;
    prev = cur;
  }
  return false;
}

const char* brush_op_name(BrushOp op) {
  switch (op) {
    case BrushOp::Raise: return "raise";
    case BrushOp::Lower: return "lower";
    case BrushOp::Smooth: return "smooth";
    case BrushOp::Flatten: return "flatten";
    case BrushOp::Noise: return "noise";
    case BrushOp::Paint: return "paint";
  }
  return "raise";
}

bool brush_op_from(const std::string& n, BrushOp* out) {
  for (BrushOp op : {BrushOp::Raise, BrushOp::Lower, BrushOp::Smooth, BrushOp::Flatten,
                     BrushOp::Noise, BrushOp::Paint}) {
    if (n == brush_op_name(op)) {
      *out = op;
      return true;
    }
  }
  return false;
}

void Rect::merge(const Rect& o) {
  if (o.empty()) return;
  if (empty()) {
    *this = o;
    return;
  }
  i0 = std::min(i0, o.i0);
  j0 = std::min(j0, o.j0);
  i1 = std::max(i1, o.i1);
  j1 = std::max(j1, o.j1);
}

float brush_weight(const Brush& b, float d) {
  if (b.radius <= 0 || d >= b.radius) return 0.0f;
  const float r = d / b.radius;
  const float inner = std::clamp(b.hardness, 0.0f, 0.99f);
  if (r <= inner) return 1.0f;
  const float x = (r - inner) / (1.0f - inner);
  return 0.5f * (1.0f + std::cos(x * render3d::kPi));
}

Rect apply_brush(TerrainData& t, const Brush& b, float x, float z, float dt) {
  Rect r;
  if (!t.valid() || b.radius <= 0 || dt <= 0) return r;
  const float rc = b.radius / t.cell();
  const float cx = t.gx(x), cz = t.gz(z);
  r.i0 = std::max(0, static_cast<int>(std::floor(cx - rc)));
  r.i1 = std::min(t.res - 1, static_cast<int>(std::ceil(cx + rc)));
  r.j0 = std::max(0, static_cast<int>(std::floor(cz - rc)));
  r.j1 = std::min(t.res - 1, static_cast<int>(std::ceil(cz + rc)));
  if (r.empty()) return r;
  const int w = r.i1 - r.i0 + 1;
  std::vector<float> blurred;
  if (b.op == BrushOp::Smooth) {
    blurred.resize(static_cast<std::size_t>(w) * (r.j1 - r.j0 + 1));
    for (int j = r.j0; j <= r.j1; ++j) {
      for (int i = r.i0; i <= r.i1; ++i) {
        float s = 0, n = 0;
        for (int dj = -1; dj <= 1; ++dj) {
          for (int di = -1; di <= 1; ++di) {
            const int ii = std::clamp(i + di, 0, t.res - 1), jj = std::clamp(j + dj, 0, t.res - 1);
            const float k = (di == 0 && dj == 0) ? 4.0f : (di == 0 || dj == 0) ? 2.0f : 1.0f;
            s += t.h(ii, jj) * k;
            n += k;
          }
        }
        blurred[static_cast<std::size_t>(j - r.j0) * w + (i - r.i0)] = s / n;
      }
    }
  }
  for (int j = r.j0; j <= r.j1; ++j) {
    for (int i = r.i0; i <= r.i1; ++i) {
      const float d = std::hypot(t.wx(i) - x, t.wz(j) - z);
      const float k = brush_weight(b, d);
      if (k <= 0) continue;
      float& h = t.h(i, j);
      const float amt = k * b.strength * dt;
      switch (b.op) {
        case BrushOp::Raise: h += amt; break;
        case BrushOp::Lower: h -= amt; break;
        case BrushOp::Smooth: {
          const float target = blurred[static_cast<std::size_t>(j - r.j0) * w + (i - r.i0)];
          h += (target - h) * std::min(1.0f, amt * 2.0f);
          break;
        }
        case BrushOp::Flatten: {
          const float f = std::min(1.0f, amt);
          h += (b.target - h) * f;
          break;
        }
        case BrushOp::Noise: {
          const float n = value_noise(t.wx(i) / std::max(0.25f, b.noise_scale),
                                      t.wz(j) / std::max(0.25f, b.noise_scale), b.seed);
          h += (n - 0.5f) * 2.0f * amt;
          break;
        }
        case BrushOp::Paint: {
          const int layer = std::clamp(b.layer, 0, kLayers - 1);
          std::uint8_t* s = &t.splat[(static_cast<std::size_t>(j) * t.res + i) * 4];
          float v[kLayers];
          float sum = 0;
          for (int l = 0; l < kLayers; ++l) sum += s[l];
          for (int l = 0; l < kLayers; ++l) v[l] = sum > 0 ? s[l] / sum : 0.0f;
          const float a = std::min(1.0f, amt * 0.25f);
          for (int l = 0; l < kLayers; ++l) v[l] = v[l] * (1.0f - a) + (l == layer ? a : 0.0f);
          // Round while keeping the sum at 255.
          int total = 0;
          int big = 0;
          for (int l = 0; l < kLayers; ++l) {
            s[l] = static_cast<std::uint8_t>(std::clamp(std::lround(v[l] * 255.0f), 0L, 255L));
            total += s[l];
            if (s[l] > s[big]) big = l;
          }
          s[big] = static_cast<std::uint8_t>(std::clamp(s[big] + (255 - total), 0, 255));
          break;
        }
      }
      h = std::clamp(h, -4000.0f, 4000.0f);
    }
  }
  if (b.op != BrushOp::Paint) refit_foliage(t, r);
  note_edit(t, r, b.op != BrushOp::Paint, b.op == BrushOp::Paint, b.op != BrushOp::Paint);
  return r;
}

void refit_foliage(TerrainData& t, const Rect& r) {
  if (r.empty() || !t.valid()) return;
  const float x0 = t.wx(r.i0) - t.cell(), x1 = t.wx(r.i1) + t.cell();
  const float z0 = t.wz(r.j0) - t.cell(), z1 = t.wz(r.j1) + t.cell();
  for (FoliageInstance& f : t.foliage) {
    if (f.x < x0 || f.x > x1 || f.z < z0 || f.z > z1) continue;
    f.y = height_at(t, f.x, f.z);
  }
}

ChunkLayout chunk_layout(const TerrainData& t, int chunk_quads) {
  ChunkLayout l;
  const int q = std::max(1, t.res - 1);
  int c = 8;
  while (c * 2 <= chunk_quads && c * 2 <= q) c *= 2;
  l.quads = std::min(c, q);
  l.count = q / l.quads;
  l.lods = 1;
  while (l.lods < 5 && (l.quads >> l.lods) >= 2) ++l.lods;
  return l;
}

MeshData build_chunk(const TerrainData& t, const ChunkLayout& l, int cx, int cz, int lod, float skirt) {
  MeshData m;
  m.name = "terrain_chunk";
  if (!t.valid()) return m;
  lod = std::clamp(lod, 0, l.lods - 1);
  const int step = 1 << lod;
  const int n = l.quads / step + 1;
  const int i_base = cx * l.quads, j_base = cz * l.quads;
  const float inv = 1.0f / static_cast<float>(t.res - 1);
  auto vertex = [&](int i, int j, float drop) {
    Vertex3D v;
    v.position = {t.wx(i), t.h(i, j) - drop, t.wz(j)};
    v.normal = grid_normal(t, i, j);
    v.u = static_cast<float>(i) * inv;
    v.v = static_cast<float>(j) * inv;
    v.tangent = {1, 0, 0, 1};
    return v;
  };
  m.vertices.reserve(static_cast<std::size_t>(n) * n + 4 * n);
  for (int b = 0; b < n; ++b)
    for (int a = 0; a < n; ++a) m.vertices.push_back(vertex(i_base + a * step, j_base + b * step, 0));
  for (int b = 0; b + 1 < n; ++b) {
    for (int a = 0; a + 1 < n; ++a) {
      const std::uint32_t i00 = static_cast<std::uint32_t>(b * n + a);
      const std::uint32_t i10 = i00 + 1, i01 = i00 + static_cast<std::uint32_t>(n), i11 = i01 + 1;
      m.indices.insert(m.indices.end(), {i00, i01, i10, i10, i01, i11});
    }
  }
  if (skirt > 0) {
    // Four edges; both windings so culling never opens a gap.
    auto edge = [&](auto grid_index) {
      const std::uint32_t base = static_cast<std::uint32_t>(m.vertices.size());
      for (int k = 0; k < n; ++k) {
        int a, b;
        grid_index(k, &a, &b);
        m.vertices.push_back(vertex(i_base + a * step, j_base + b * step, skirt));
      }
      for (int k = 0; k + 1 < n; ++k) {
        int a0, b0, a1, b1;
        grid_index(k, &a0, &b0);
        grid_index(k + 1, &a1, &b1);
        const std::uint32_t t0 = static_cast<std::uint32_t>(b0 * n + a0);
        const std::uint32_t t1 = static_cast<std::uint32_t>(b1 * n + a1);
        const std::uint32_t s0 = base + static_cast<std::uint32_t>(k), s1 = s0 + 1;
        m.indices.insert(m.indices.end(), {t0, s0, t1, t1, s0, s1, t0, t1, s0, t1, s1, s0});
      }
    };
    edge([&](int k, int* a, int* b) { *a = k; *b = 0; });
    edge([&](int k, int* a, int* b) { *a = k; *b = n - 1; });
    edge([&](int k, int* a, int* b) { *a = 0; *b = k; });
    edge([&](int k, int* a, int* b) { *a = n - 1; *b = k; });
  }
  return m;
}

int select_lod(float distance, const float* distances, int count, int lods) {
  int lod = 0;
  for (int i = 0; i < count; ++i)
    if (distance >= distances[i]) lod = i + 1;
  return std::clamp(lod, 0, std::max(0, lods - 1));
}

void chunks_in(const ChunkLayout& l, const Rect& r, std::vector<int>* out) {
  out->clear();
  if (r.empty() || l.count <= 0) return;
  // A vertex on a chunk border belongs to both neighbours (normals too).
  const int c0 = std::clamp((r.i0 - 1) / l.quads, 0, l.count - 1);
  const int c1 = std::clamp((r.i1 + 1) / l.quads, 0, l.count - 1);
  const int d0 = std::clamp((r.j0 - 1) / l.quads, 0, l.count - 1);
  const int d1 = std::clamp((r.j1 + 1) / l.quads, 0, l.count - 1);
  for (int cz = d0; cz <= d1; ++cz)
    for (int cx = c0; cx <= c1; ++cx) out->push_back(cz * l.count + cx);
}

Aabb chunk_bounds(const TerrainData& t, const ChunkLayout& l, int cx, int cz) {
  Aabb b{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
  if (!t.valid()) return {};
  for (int j = cz * l.quads; j <= (cz + 1) * l.quads && j < t.res; ++j) {
    for (int i = cx * l.quads; i <= (cx + 1) * l.quads && i < t.res; ++i) {
      const Vec3 p{t.wx(i), t.h(i, j), t.wz(j)};
      b.min = render3d::vmin(b.min, p);
      b.max = render3d::vmax(b.max, p);
    }
  }
  return b;
}

Image splat_image(const TerrainData& t) {
  Image img;
  if (!t.valid()) return img;
  img.width = img.height = t.res;
  img.rgba = t.splat;
  return img;
}

const std::vector<std::string>& builtin_layer_names() {
  static const std::vector<std::string> names = {"builtin:grass", "builtin:dirt", "builtin:rock",
                                                 "builtin:sand", "builtin:snow"};
  return names;
}

bool builtin_layer_image(const std::string& name, Image* out) {
  struct Look {
    const char* name;
    float a[3], b[3];
    float feature, streak;
  };
  static const Look looks[] = {
      {"builtin:grass", {0.30f, 0.42f, 0.16f}, {0.46f, 0.55f, 0.22f}, 6.0f, 0.0f},
      {"builtin:dirt", {0.38f, 0.27f, 0.17f}, {0.55f, 0.42f, 0.28f}, 5.0f, 0.0f},
      {"builtin:rock", {0.36f, 0.34f, 0.32f}, {0.60f, 0.57f, 0.52f}, 9.0f, 1.0f},
      {"builtin:sand", {0.76f, 0.62f, 0.42f}, {0.88f, 0.76f, 0.55f}, 7.0f, 0.5f},
      {"builtin:snow", {0.84f, 0.87f, 0.92f}, {0.98f, 0.98f, 1.00f}, 8.0f, 0.0f},
  };
  const Look* look = nullptr;
  for (const Look& l : looks)
    if (name == l.name) look = &l;
  if (!look) return false;
  const int n = 128;
  out->width = out->height = n;
  out->rgba.resize(static_cast<std::size_t>(n) * n * 4);
  const std::uint32_t seed = hash3(static_cast<std::uint32_t>(name.size()),
                                   static_cast<std::uint32_t>(name.back()), 7);
  // Tileable: lattice wraps every n * f cells.
  auto tile_noise = [&](float x, float y, float f, std::uint32_t s) {
    const float px = x * f, py = y * f;
    const float fx = std::floor(px), fy = std::floor(py);
    float tx = px - fx, ty = py - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    const int per = std::max(1, static_cast<int>(std::lround(static_cast<float>(n) * f)));
    auto v = [&](int a, int b) {
      a = ((a % per) + per) % per;
      b = ((b % per) + per) % per;
      return hash_unit(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b), s);
    };
    const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
    const float a = v(ix, iy) + (v(ix + 1, iy) - v(ix, iy)) * tx;
    const float b = v(ix, iy + 1) + (v(ix + 1, iy + 1) - v(ix, iy + 1)) * tx;
    return a + (b - a) * ty;
  };
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      float v = 0, amp = 0.5f, f = look->feature / static_cast<float>(n);
      for (int o = 0; o < 4; ++o) {
        v += tile_noise(static_cast<float>(x), static_cast<float>(y), f, seed + static_cast<std::uint32_t>(o)) * amp;
        amp *= 0.5f;
        f *= 2.0f;
      }
      v = std::clamp(v / 0.9375f, 0.0f, 1.0f);
      const float grain = hash_unit(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), seed ^ 0xABCu);
      if (look->streak > 0) v = v * 0.7f + 0.3f * (0.5f + 0.5f * std::sin((y + v * 18.0f) * 0.35f)) * look->streak;
      const float k = std::clamp(v * 0.85f + grain * 0.15f, 0.0f, 1.0f);
      unsigned char* p = &out->rgba[(static_cast<std::size_t>(y) * n + x) * 4];
      for (int c = 0; c < 3; ++c)
        p[c] = static_cast<unsigned char>(std::clamp((look->a[c] + (look->b[c] - look->a[c]) * k) * 255.0f, 0.0f, 255.0f));
      p[3] = 255;
    }
  }
  return true;
}

namespace {
constexpr char kMagic[4] = {'T', 'S', 'T', 'R'};
constexpr std::uint32_t kFileVersion = 1;

template <typename T>
void put(std::string& s, const T& v) {
  s.append(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <typename T>
bool get(const std::string& s, std::size_t* at, T* v) {
  if (*at + sizeof(T) > s.size()) return false;
  std::memcpy(v, s.data() + *at, sizeof(T));
  *at += sizeof(T);
  return true;
}
}  // namespace

std::string encode(const TerrainData& t) {
  std::string s;
  s.append(kMagic, 4);
  put(s, kFileVersion);
  put(s, static_cast<std::int32_t>(t.res));
  put(s, t.size);
  s.append(reinterpret_cast<const char*>(t.heights.data()), t.heights.size() * sizeof(float));
  s.append(reinterpret_cast<const char*>(t.splat.data()), t.splat.size());
  put(s, static_cast<std::uint32_t>(t.foliage.size()));
  for (const FoliageInstance& f : t.foliage) {
    put(s, f.type);
    put(s, static_cast<std::uint16_t>(0));
    put(s, f.x);
    put(s, f.z);
    put(s, f.yaw);
    put(s, f.scale);
  }
  return s;
}

bool decode(const std::string& s, TerrainData* out, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  if (s.size() < 16 || std::memcmp(s.data(), kMagic, 4) != 0) return fail("not a .tsterrain file");
  std::size_t at = 4;
  std::uint32_t version = 0;
  std::int32_t res = 0;
  float size = 0;
  get(s, &at, &version);
  get(s, &at, &res);
  get(s, &at, &size);
  if (version != kFileVersion) return fail("unsupported .tsterrain version");
  if (!valid_resolution(res) || !(size > 0) || size > 8192.0f) return fail("bad terrain resolution / size");
  TerrainData t;
  t.res = res;
  t.size = size;
  const std::size_t n = static_cast<std::size_t>(res) * res;
  if (at + n * sizeof(float) + n * 4 + 4 > s.size()) return fail("truncated .tsterrain");
  t.heights.resize(n);
  std::memcpy(t.heights.data(), s.data() + at, n * sizeof(float));
  at += n * sizeof(float);
  t.splat.assign(s.begin() + static_cast<std::ptrdiff_t>(at), s.begin() + static_cast<std::ptrdiff_t>(at + n * 4));
  at += n * 4;
  for (float& h : t.heights)
    if (!std::isfinite(h)) h = 0;
  std::uint32_t count = 0;
  get(s, &at, &count);
  if (count > 4000000u || at + static_cast<std::size_t>(count) * 20 > s.size()) return fail("truncated foliage");
  t.foliage.resize(count);
  for (FoliageInstance& f : t.foliage) {
    std::uint16_t pad = 0;
    get(s, &at, &f.type);
    get(s, &at, &pad);
    get(s, &at, &f.x);
    get(s, &at, &f.z);
    get(s, &at, &f.yaw);
    get(s, &at, &f.scale);
    f.y = height_at(t, f.x, f.z);
  }
  t.revision = out->revision + 1;  // no edit log: caches rebuild
  *out = std::move(t);
  return true;
}

bool save_file(const std::string& path, const TerrainData& t, std::string* error) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (error) *error = "cannot write " + path;
    return false;
  }
  const std::string bytes = encode(t);
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!f) {
    if (error) *error = "write failed: " + path;
    return false;
  }
  return true;
}

bool load_file(const std::string& path, TerrainData* out, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return decode(ss.str(), out, error);
}

}  // namespace terrain
}  // namespace tombstone
}  // namespace ts
