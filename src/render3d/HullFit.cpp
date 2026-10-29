#include "render3d/HullFit.h"

#include "core/AssetMeta.h"
#include "render3d/ModelImport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <tuple>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

namespace {

constexpr float kHullEps = 1.0e-5f;

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

struct Tri {
  Vec3 a, b, c;
};

float dist2(const Vec3& a, const Vec3& b) {
  const Vec3 d = a - b;
  return dot(d, d);
}

std::uint64_t fnv_mix(std::uint64_t h, std::uint64_t v) {
  h ^= v;
  return h * 0x100000001b3ull;
}

int quant(float v) { return static_cast<int>(std::lround(static_cast<double>(v) * 10000.0)); }

struct Face {
  int v[3]{};
  Vec3 n;
  float d = 0.0f;  // dot(n, x) = d, n points outward
  bool dead = false;
};

float face_dist(const Face& f, const Vec3& p) { return dot(f.n, p) - f.d; }

void set_face(Face* f, const std::vector<Vec3>& pts, int a, int b, int c, const Vec3& inside) {
  f->v[0] = a;
  f->v[1] = b;
  f->v[2] = c;
  f->dead = false;
  const Vec3 n = cross(pts[b] - pts[a], pts[c] - pts[a]);
  const float len = length(n);
  f->n = len > 1.0e-12f ? n * (1.0f / len) : Vec3{0, 1, 0};
  f->d = dot(f->n, pts[a]);
  if (face_dist(*f, inside) > 0.0f) {
    std::swap(f->v[1], f->v[2]);
    f->n = f->n * -1.0f;
    f->d = -f->d;
  }
}

// Horizon edge of a visible face, directed as that face walks it (a -> b).
struct Hedge {
  int a = 0, b = 0;
};


}  // namespace

// The face builder above got tangled with a discarded compact step. The
// functions the rest of the file call are defined here, in full, so the
// hull, the wireframe and the "is this point inside" test share one path.

namespace {

struct HullBuild {
  std::vector<Vec3> points;
  std::vector<Face> faces;
};

HullBuild build_hull(std::vector<Vec3> pts) {
  HullBuild out;
  if (pts.size() > 5000) {
    std::vector<Vec3> slim;
    slim.reserve(5000);
    const std::size_t step = pts.size() / 5000;
    for (std::size_t i = 0; i < pts.size(); i += std::max<std::size_t>(step, 1)) {
      const Vec3& p = pts[i];
      if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) slim.push_back(p);
    }
    pts.swap(slim);
  }
  std::vector<Vec3> welded;
  const float weld2 = 1.0e-10f;
  for (const Vec3& p : pts) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
    bool dup = false;
    for (const Vec3& q : welded) {
      if (dist2(p, q) <= weld2) {
        dup = true;
        break;
      }
    }
    if (!dup) welded.push_back(p);
  }
  pts.swap(welded);
  if (pts.size() < 4) {
    if (pts.empty()) return out;
    while (pts.size() < 3) pts.push_back(pts.back() + Vec3{0.01f * static_cast<float>(pts.size() + 1), 0, 0});
    Vec3 n = cross(pts[1] - pts[0], pts[2] - pts[0]);
    if (length(n) < 1.0e-8f) n = {0, 1, 0};
    pts.push_back(pts[0] + normalize(n) * 0.01f);
  }
  int i0 = 0, i1 = 1;
  float best = -1.0f;
  for (int i = 0; i < static_cast<int>(pts.size()); ++i)
    for (int j = i + 1; j < static_cast<int>(pts.size()); ++j) {
      const float d = dist2(pts[i], pts[j]);
      if (d > best) {
        best = d;
        i0 = i;
        i1 = j;
      }
    }
  const Vec3 axis = pts[i1] - pts[i0];
  int i2 = 0;
  best = -1.0f;
  for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
    if (i == i0 || i == i1) continue;
    const float d = length(cross(axis, pts[i] - pts[i0]));
    if (d > best) {
      best = d;
      i2 = i;
    }
  }
  const Vec3 pn = cross(pts[i1] - pts[i0], pts[i2] - pts[i0]);
  const float pd = dot(pn, pts[i0]);
  int i3 = 0;
  best = -1.0f;
  for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
    if (i == i0 || i == i1 || i == i2) continue;
    const float d = std::fabs(dot(pn, pts[i]) - pd);
    if (d > best) {
      best = d;
      i3 = i;
    }
  }
  if (best < 1.0e-8f) {
    Vec3 n = length(pn) < 1.0e-8f ? Vec3{0, 1, 0} : normalize(pn);
    pts.push_back(pts[i0] + n * 0.01f);
    i3 = static_cast<int>(pts.size()) - 1;
  }
  const Vec3 inside = (pts[i0] + pts[i1] + pts[i2] + pts[i3]) * 0.25f;
  std::vector<Face> faces(4);
  set_face(&faces[0], pts, i0, i1, i2, inside);
  set_face(&faces[1], pts, i0, i2, i3, inside);
  set_face(&faces[2], pts, i0, i3, i1, inside);
  set_face(&faces[3], pts, i1, i3, i2, inside);
  float diag = 0.0f;
  for (const Vec3& p : pts) diag = std::max(diag, length(p - pts[i0]));
  const float eps = std::max(kHullEps, diag * 1.0e-5f);
  std::vector<char> used(pts.size(), 0);
  used[i0] = used[i1] = used[i2] = used[i3] = 1;
  std::vector<std::vector<int>> outside(faces.size());
  auto assign = [&](int idx, std::size_t from) {
    if (used[idx]) return;
    for (std::size_t f = from; f < faces.size(); ++f) {
      if (faces[f].dead) continue;
      if (face_dist(faces[f], pts[idx]) > eps) {
        outside[f].push_back(idx);
        return;
      }
    }
  };
  for (int i = 0; i < static_cast<int>(pts.size()); ++i) assign(i, 0);

  for (int guard = 0; guard < static_cast<int>(pts.size()) + 2; ++guard) {
    int sf = -1, point = -1;
    float furthest = eps;
    for (std::size_t f = 0; f < faces.size(); ++f) {
      if (faces[f].dead) continue;
      for (int idx : outside[f]) {
        const float d = face_dist(faces[f], pts[idx]);
        if (d > furthest) {
          furthest = d;
          sf = static_cast<int>(f);
          point = idx;
        }
      }
    }
    if (sf < 0) break;
    std::vector<char> visible(faces.size(), 0);
    std::vector<int> stack{sf};
    visible[sf] = 1;
    while (!stack.empty()) {
      const int f = stack.back();
      stack.pop_back();
      for (std::size_t g = 0; g < faces.size(); ++g) {
        if (visible[g] || faces[g].dead || static_cast<int>(g) == f) continue;
        bool share = false;
        for (int e = 0; e < 3 && !share; ++e) {
          const int a = faces[f].v[e], b = faces[f].v[(e + 1) % 3];
          for (int k = 0; k < 3; ++k)
            if (faces[g].v[k] == b && faces[g].v[(k + 1) % 3] == a) share = true;
        }
        if (share && face_dist(faces[g], pts[point]) > eps) {
          visible[g] = 1;
          stack.push_back(static_cast<int>(g));
        }
      }
    }
    std::vector<Hedge> horizon;
    for (std::size_t f = 0; f < faces.size(); ++f) {
      if (!visible[f]) continue;
      for (int e = 0; e < 3; ++e) {
        const int a = faces[f].v[e], b = faces[f].v[(e + 1) % 3];
        bool boundary = true;
        for (std::size_t g = 0; g < faces.size() && boundary; ++g) {
          if (!visible[g] || faces[g].dead || g == f) continue;
          for (int k = 0; k < 3; ++k)
            if (faces[g].v[k] == b && faces[g].v[(k + 1) % 3] == a) boundary = false;
        }
        if (boundary) horizon.push_back({a, b});
      }
    }
    std::vector<int> orphans;
    for (std::size_t f = 0; f < faces.size(); ++f) {
      if (!visible[f]) continue;
      faces[f].dead = true;
      orphans.insert(orphans.end(), outside[f].begin(), outside[f].end());
      outside[f].clear();
    }
    used[point] = 1;
    const std::size_t first = faces.size();
    for (const Hedge& h : horizon) {
      Face nf;
      set_face(&nf, pts, h.b, h.a, point, inside);
      faces.push_back(nf);
      outside.emplace_back();
    }
    for (int idx : orphans) assign(idx, first);
  }

  std::vector<int> remap(pts.size(), -1);
  for (const Face& f : faces) {
    if (f.dead) continue;
    Face g = f;
    for (int k = 0; k < 3; ++k) {
      int& slot = remap[f.v[k]];
      if (slot < 0) {
        slot = static_cast<int>(out.points.size());
        out.points.push_back(pts[f.v[k]]);
      }
      g.v[k] = slot;
    }
    // Recompute the plane against the compacted points (same positions).
    set_face(&g, out.points, g.v[0], g.v[1], g.v[2], inside);
    out.faces.push_back(g);
  }
  if (out.points.size() < 4 && !pts.empty()) {
    out.points = pts;
    out.faces.clear();
  }
  return out;
}

std::vector<Vec3> limit_cloud(std::vector<Vec3> src, int limit) {
  if (limit <= 0 || static_cast<int>(src.size()) <= limit) return src;
  const int keep = std::max(limit, 4);
  while (static_cast<int>(src.size()) > keep) {
    float best = std::numeric_limits<float>::max();
    int drop = static_cast<int>(src.size()) - 1;
    const int n = static_cast<int>(src.size());
    const int step = std::max(1, n / 48);
    for (int i = 0; i < n; i += step) {
      for (int j = i + step; j < n; j += step) {
        const float d = dist2(src[i], src[j]);
        if (d < best) {
          best = d;
          drop = j;
        }
      }
    }
    src.erase(src.begin() + drop);
  }
  return src;
}

float tri_volume(const Tri& t) { return dot(t.a, cross(t.b, t.c)) / 6.0f; }

float mesh_volume(const std::vector<Tri>& tris) {
  float v = 0.0f;
  for (const Tri& t : tris) v += tri_volume(t);
  return std::fabs(v);
}

float hull_volume(const HullBuild& h) {
  float v = 0.0f;
  for (const Face& f : h.faces) {
    const Vec3& a = h.points[f.v[0]];
    const Vec3& b = h.points[f.v[1]];
    const Vec3& c = h.points[f.v[2]];
    v += dot(a, cross(b, c));
  }
  return std::fabs(v) / 6.0f;
}

void jacobi(float a[3][3], float v[3][3]) {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) v[r][c] = r == c ? 1.0f : 0.0f;
  for (int iter = 0; iter < 24; ++iter) {
    int p = 0, q = 1;
    float maxv = std::fabs(a[0][1]);
    if (std::fabs(a[0][2]) > maxv) {
      maxv = std::fabs(a[0][2]);
      p = 0;
      q = 2;
    }
    if (std::fabs(a[1][2]) > maxv) {
      maxv = std::fabs(a[1][2]);
      p = 1;
      q = 2;
    }
    if (maxv < 1.0e-10f) break;
    const float app = a[p][p], aqq = a[q][q], apq = a[p][q];
    const float tau = (aqq - app) / (2.0f * apq);
    const float t = std::copysign(1.0f / (std::fabs(tau) + std::sqrt(1.0f + tau * tau)), tau);
    const float c = 1.0f / std::sqrt(1.0f + t * t);
    const float s = t * c;
    a[p][p] = app - t * apq;
    a[q][q] = aqq + t * apq;
    a[p][q] = a[q][p] = 0.0f;
    for (int r = 0; r < 3; ++r) {
      if (r == p || r == q) continue;
      const float arp = a[r][p], arq = a[r][q];
      a[r][p] = a[p][r] = c * arp - s * arq;
      a[r][q] = a[q][r] = s * arp + c * arq;
    }
    for (int r = 0; r < 3; ++r) {
      const float vip = v[r][p], viq = v[r][q];
      v[r][p] = c * vip - s * viq;
      v[r][q] = s * vip + c * viq;
    }
  }
}

Quat quat_from_axes(Vec3 x, Vec3 y, Vec3 z) {
  // Columns of the rotation (local axis -> world).
  const float m00 = x.x, m01 = y.x, m02 = z.x;
  const float m10 = x.y, m11 = y.y, m12 = z.y;
  const float m20 = x.z, m21 = y.z, m22 = z.z;
  Quat q;
  const float tr = m00 + m11 + m22;
  if (tr > 0.0f) {
    const float s = std::sqrt(tr + 1.0f) * 2.0f;
    q.w = 0.25f * s;
    q.x = (m21 - m12) / s;
    q.y = (m02 - m20) / s;
    q.z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
    q.w = (m21 - m12) / s;
    q.x = 0.25f * s;
    q.y = (m01 + m10) / s;
    q.z = (m02 + m20) / s;
  } else if (m11 > m22) {
    const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
    q.w = (m02 - m20) / s;
    q.x = (m01 + m10) / s;
    q.y = 0.25f * s;
    q.z = (m12 + m21) / s;
  } else {
    const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    q.w = (m10 - m01) / s;
    q.x = (m02 + m20) / s;
    q.y = (m12 + m21) / s;
    q.z = 0.25f * s;
  }
  return normalize(q);
}

bool collect_mesh(const Model& m, std::vector<Tri>* tris, std::vector<Vec3>* pts) {
  MeshData mesh;
  rest_pose_mesh(m, &mesh);
  if (!mesh.valid()) return false;
  tris->clear();
  pts->clear();
  tris->reserve(mesh.indices.size() / 3);
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const std::uint32_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
    if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size()) continue;
    Tri t{mesh.vertices[ia].position, mesh.vertices[ib].position, mesh.vertices[ic].position};
    if (length(cross(t.b - t.a, t.c - t.a)) < 1.0e-12f) continue;
    tris->push_back(t);
    pts->push_back(t.a);
    pts->push_back(t.b);
    pts->push_back(t.c);
  }
  return !tris->empty();
}

struct QKey {
  int x, y, z;
  bool operator==(const QKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct QHash {
  std::size_t operator()(const QKey& k) const {
    return (static_cast<std::size_t>(k.x) * 73856093u) ^ (static_cast<std::size_t>(k.y) * 19349663u) ^
           (static_cast<std::size_t>(k.z) * 83492791u);
  }
};

std::vector<Vec3> dedup_points(const std::vector<Vec3>& pts) {
  std::vector<Vec3> out;
  std::unordered_map<QKey, int, QHash> seen;
  out.reserve(pts.size());
  for (const Vec3& p : pts) {
    const QKey k{quant(p.x), quant(p.y), quant(p.z)};
    if (seen.emplace(k, 1).second) out.push_back(p);
  }
  return out;
}

float concavity_of(const std::vector<Tri>& tris) {
  if (tris.size() <= 1) return 0.0f;
  const float mv = mesh_volume(tris);
  std::vector<Vec3> pts;
  pts.reserve(tris.size() * 3);
  for (const Tri& t : tris) {
    pts.push_back(t.a);
    pts.push_back(t.b);
    pts.push_back(t.c);
  }
  const HullBuild h = build_hull(dedup_points(pts));
  const float hv = hull_volume(h);
  if (hv < 1.0e-8f || mv < 1.0e-8f) return 0.0f;
  return std::max(0.0f, 1.0f - mv / hv);
}

struct EdgeKey {
  QKey a, b;
  bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};
struct EdgeHash {
  std::size_t operator()(const EdgeKey& e) const {
    QHash h;
    return h(e.a) ^ (h(e.b) * 16777619u);
  }
};
QKey qkey(const Vec3& p) { return {quant(p.x), quant(p.y), quant(p.z)}; }
EdgeKey ekey(const Vec3& a, const Vec3& b) {
  QKey ka = qkey(a), kb = qkey(b);
  const bool swap = std::tie(ka.x, ka.y, ka.z) > std::tie(kb.x, kb.y, kb.z);
  return swap ? EdgeKey{kb, ka} : EdgeKey{ka, kb};
}

std::vector<std::vector<Tri>> decompose_tris(const std::vector<Tri>& tris, const FitParams& params) {
  const int n = static_cast<int>(tris.size());
  if (n == 0) return {};
  if (n == 1 || params.decompose_max <= 1) return {tris};

  // A dense mesh: bucket triangles on a grid and fold buckets down to the
  // piece limit. The careful merge below is for the props people actually
  // fit by hand; a 20k-triangle rock should not stall the import.
  if (n > 500) {
    Vec3 mn{1.0e30f, 1.0e30f, 1.0e30f}, mx{-1.0e30f, -1.0e30f, -1.0e30f};
    std::vector<Vec3> centre(n);
    for (int i = 0; i < n; ++i) {
      centre[i] = (tris[i].a + tris[i].b + tris[i].c) * (1.0f / 3.0f);
      mn = vmin(mn, centre[i]);
      mx = vmax(mx, centre[i]);
    }
    Vec3 ext = mx - mn;
    if (ext.x < 1.0e-4f) ext.x = 1.0f;
    if (ext.y < 1.0e-4f) ext.y = 1.0f;
    if (ext.z < 1.0e-4f) ext.z = 1.0f;
    constexpr int kGrid = 4;
    auto cell_of = [&](const Vec3& p) {
      auto ax = [&](float v, float lo, float span) {
        return std::clamp(static_cast<int>((v - lo) / span * kGrid), 0, kGrid - 1);
      };
      return ax(p.x, mn.x, ext.x) + kGrid * (ax(p.y, mn.y, ext.y) + kGrid * ax(p.z, mn.z, ext.z));
    };
    std::unordered_map<int, std::vector<Tri>> cells;
    for (int i = 0; i < n; ++i) cells[cell_of(centre[i])].push_back(tris[i]);
    std::vector<std::vector<Tri>> pieces;
    pieces.reserve(cells.size());
    for (auto& [id, group] : cells) {
      (void)id;
      pieces.push_back(std::move(group));
    }
    const int cap = std::max(1, params.decompose_max);
    while (static_cast<int>(pieces.size()) > cap) {
      std::size_t a = 0, b = 1;
      if (pieces[b].size() < pieces[a].size()) std::swap(a, b);
      for (std::size_t i = 2; i < pieces.size(); ++i) {
        if (pieces[i].size() < pieces[a].size()) {
          b = a;
          a = i;
        } else if (pieces[i].size() < pieces[b].size()) {
          b = i;
        }
      }
      if (a > b) std::swap(a, b);
      pieces[a].insert(pieces[a].end(), pieces[b].begin(), pieces[b].end());
      pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(b));
    }
    return pieces;
  }

  std::vector<std::vector<int>> members(n);
  for (int i = 0; i < n; ++i) members[i] = {i};
  std::vector<char> alive(n, 1);
  std::vector<int> stamp(n, 0);
  int alive_n = n;

  std::unordered_map<EdgeKey, std::vector<int>, EdgeHash> edge_tris;
  for (int i = 0; i < n; ++i) {
    const Tri& t = tris[i];
    edge_tris[ekey(t.a, t.b)].push_back(i);
    edge_tris[ekey(t.b, t.c)].push_back(i);
    edge_tris[ekey(t.c, t.a)].push_back(i);
  }
  std::vector<std::vector<int>> neigh(n);
  for (const auto& [e, ids] : edge_tris) {
    (void)e;
    for (std::size_t a = 0; a < ids.size(); ++a)
      for (std::size_t b = a + 1; b < ids.size(); ++b) {
        neigh[ids[a]].push_back(ids[b]);
        neigh[ids[b]].push_back(ids[a]);
      }
  }

  auto cluster_tris = [&](int id) {
    std::vector<Tri> out;
    out.reserve(members[id].size());
    for (int t : members[id]) out.push_back(tris[t]);
    return out;
  };

  struct Link {
    float c;
    int a, b, sa, sb;
    bool operator>(const Link& o) const { return c > o.c; }
  };
  std::priority_queue<Link, std::vector<Link>, std::greater<Link>> heap;
  auto push_link = [&](int a, int b) {
    if (a == b || !alive[a] || !alive[b]) return;
    if (a > b) std::swap(a, b);
    std::vector<Tri> u = cluster_tris(a);
    const std::vector<Tri> vb = cluster_tris(b);
    u.insert(u.end(), vb.begin(), vb.end());
    heap.push({concavity_of(u), a, b, stamp[a], stamp[b]});
  };
  for (int i = 0; i < n; ++i)
    for (int j : neigh[i])
      if (i < j) push_link(i, j);

  const int cap = std::max(1, params.decompose_max);
  const float stop_at = std::max(0.0f, params.concavity);
  int safety = n * 2 + 8;
  while (!heap.empty() && alive_n > 1 && safety-- > 0) {
    const Link link = heap.top();
    heap.pop();
    if (!alive[link.a] || !alive[link.b] || stamp[link.a] != link.sa || stamp[link.b] != link.sb) continue;
    if (link.c > stop_at && alive_n <= cap) break;
    // Fold b into a.
    members[link.a].insert(members[link.a].end(), members[link.b].begin(), members[link.b].end());
    alive[link.b] = 0;
    --alive_n;
    ++stamp[link.a];
    std::vector<int> merged = neigh[link.a];
    merged.insert(merged.end(), neigh[link.b].begin(), neigh[link.b].end());
    std::sort(merged.begin(), merged.end());
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
    neigh[link.a].clear();
    for (int j : merged) {
      if (j == link.a || j == link.b || !alive[j]) continue;
      neigh[link.a].push_back(j);
      for (int& slot : neigh[j])
        if (slot == link.b) slot = link.a;
      std::sort(neigh[j].begin(), neigh[j].end());
      neigh[j].erase(std::unique(neigh[j].begin(), neigh[j].end()), neigh[j].end());
      push_link(link.a, j);
    }
  }

  std::vector<std::vector<Tri>> pieces;
  for (int i = 0; i < n; ++i) {
    if (!alive[i]) continue;
    pieces.push_back(cluster_tris(i));
  }
  return pieces;
}

json::Value vec3_json(const Vec3& v) {
  const float f[3] = {v.x, v.y, v.z};
  return json::floats(f, 3);
}
json::Value quat_json(const Quat& q) {
  const float f[4] = {q.x, q.y, q.z, q.w};
  return json::floats(f, 4);
}
bool read_vec3(const json::Value& v, Vec3* o) {
  if (!v.is_array() || v.size() < 3) return false;
  *o = {v[0].as_float(), v[1].as_float(), v[2].as_float()};
  return std::isfinite(o->x) && std::isfinite(o->y) && std::isfinite(o->z);
}
json::Value hulls_json(const std::vector<std::vector<Vec3>>& hulls) {
  json::Value a = json::Value::array();
  for (const auto& h : hulls) {
    json::Value p = json::Value::array();
    for (const Vec3& v : h) p.push(vec3_json(v));
    a.push(std::move(p));
  }
  return a;
}
bool read_hulls(const json::Value& a, std::vector<std::vector<Vec3>>* out) {
  if (!a.is_array()) return false;
  out->clear();
  for (const json::Value& h : a.items()) {
    if (!h.is_array()) return false;
    std::vector<Vec3> pts;
    for (const json::Value& p : h.items()) {
      Vec3 v;
      if (!read_vec3(p, &v)) return false;
      pts.push_back(v);
    }
    if (pts.size() >= 4) out->push_back(std::move(pts));
  }
  return !out->empty();
}

FittedCollision compute_fitted(const Model& m, const FitParams& params) {
  FittedCollision f;
  const FitParams p = clamp_fit_params(params);
  std::vector<Tri> tris;
  std::vector<Vec3> pts;
  if (!collect_mesh(m, &tris, &pts)) return f;
  f.fingerprint = mesh_fingerprint(m);
  f.vertex_limit = p.hull_vertices;
  f.decompose_max = p.decompose_max;

  const std::vector<Vec3> unique = dedup_points(pts);
  const HullBuild hull = build_hull(limit_cloud(unique, p.hull_vertices));
  if (hull.points.size() >= 4) f.convex.push_back(hull.points);
  else if (unique.size() >= 4) f.convex.push_back(limit_cloud(unique, p.hull_vertices));

  // Oriented box from the covariance of the drawn vertices. A cube's
  // axes are a coin toss, so the caller keeps it only when it is tighter
  // than the axis-aligned box.
  Vec3 mean;
  for (const Vec3& v : unique) mean += v;
  if (!unique.empty()) mean = mean * (1.0f / static_cast<float>(unique.size()));
  float c[3][3] = {};
  for (const Vec3& v : unique) {
    const Vec3 d = v - mean;
    const float xyz[3] = {d.x, d.y, d.z};
    for (int r = 0; r < 3; ++r)
      for (int col = 0; col < 3; ++col) c[r][col] += xyz[r] * xyz[col];
  }
  float rot[3][3];
  jacobi(c, rot);
  Vec3 ax{rot[0][0], rot[1][0], rot[2][0]};
  Vec3 ay{rot[0][1], rot[1][1], rot[2][1]};
  Vec3 az{rot[0][2], rot[1][2], rot[2][2]};
  ax = normalize(ax);
  ay = normalize(ay);
  az = cross(ax, ay);
  if (length(az) < 1.0e-6f) az = {0, 0, 1};
  az = normalize(az);
  ay = normalize(cross(az, ax));
  if (dot(cross(ax, ay), az) < 0.0f) az = az * -1.0f;
  Vec3 mn{1.0e30f, 1.0e30f, 1.0e30f}, mx{-1.0e30f, -1.0e30f, -1.0e30f};
  for (const Vec3& v : unique) {
    const Vec3 d = v - mean;
    const float lx = dot(d, ax), ly = dot(d, ay), lz = dot(d, az);
    mn.x = std::min(mn.x, lx);
    mn.y = std::min(mn.y, ly);
    mn.z = std::min(mn.z, lz);
    mx.x = std::max(mx.x, lx);
    mx.y = std::max(mx.y, ly);
    mx.z = std::max(mx.z, lz);
  }
  const Vec3 mid = (mn + mx) * 0.5f;
  f.obb_center = mean + ax * mid.x + ay * mid.y + az * mid.z;
  f.obb_extents = {std::max(0.02f, mx.x - mn.x), std::max(0.02f, mx.y - mn.y), std::max(0.02f, mx.z - mn.z)};
  f.obb_rotation = quat_from_axes(ax, ay, az);
  f.has_obb = true;

  const std::vector<std::vector<Tri>> groups = decompose_tris(tris, p);
  for (const auto& g : groups) {
    std::vector<Vec3> gp;
    gp.reserve(g.size() * 3);
    for (const Tri& t : g) {
      gp.push_back(t.a);
      gp.push_back(t.b);
      gp.push_back(t.c);
    }
    const HullBuild piece = build_hull(limit_cloud(dedup_points(gp), p.hull_vertices));
    if (piece.points.size() >= 4) f.pieces.push_back(piece.points);
  }
  if (f.pieces.empty() && !f.convex.empty()) f.pieces = f.convex;
  return f;
}

}  // namespace

FitParams clamp_fit_params(FitParams p) {
  p.hull_vertices = std::clamp(p.hull_vertices, 4, 256);
  p.decompose_max = std::clamp(p.decompose_max, 1, 32);
  if (!std::isfinite(p.concavity)) p.concavity = 0.06f;
  p.concavity = clampf(p.concavity, 0.0f, 0.5f);
  return p;
}

FitParams fit_params_from_settings(const json::Value& settings) {
  FitParams p;
  if (settings.is_object()) {
    if (settings.has("hull_vertices")) p.hull_vertices = static_cast<int>(settings.num("hull_vertices", p.hull_vertices));
    if (settings.has("decompose_max")) p.decompose_max = static_cast<int>(settings.num("decompose_max", p.decompose_max));
    if (settings.has("concavity")) p.concavity = settings.flt("concavity", p.concavity);
  }
  return clamp_fit_params(p);
}

std::uint64_t mesh_fingerprint(const Model& m) {
  MeshData mesh;
  rest_pose_mesh(m, &mesh);
  std::uint64_t h = 0xcbf29ce484222325ull;
  h = fnv_mix(h, mesh.indices.size());
  for (std::uint32_t i : mesh.indices) {
    if (i >= mesh.vertices.size()) continue;
    const Vec3& p = mesh.vertices[i].position;
    h = fnv_mix(h, static_cast<std::uint32_t>(quant(p.x)));
    h = fnv_mix(h, static_cast<std::uint32_t>(quant(p.y)));
    h = fnv_mix(h, static_cast<std::uint32_t>(quant(p.z)));
  }
  return h;
}

bool ensure_fitted(Model* m, const FitParams& params) {
  if (!m) return false;
  const FitParams p = clamp_fit_params(params);
  const std::uint64_t fp = mesh_fingerprint(*m);
  if (m->fitted.matches(fp, p.hull_vertices, p.decompose_max)) return true;
  m->fitted = compute_fitted(*m, p);
  return m->fitted.valid();
}

json::Value fitted_to_json(const FittedCollision& f) {
  json::Value o = json::Value::object();
  o.set("fingerprint", json::Value::string(std::to_string(f.fingerprint)));
  o.set("vertex_limit", json::Value::number(f.vertex_limit));
  o.set("decompose_max", json::Value::number(f.decompose_max));
  if (f.has_obb) {
    json::Value obb = json::Value::object();
    obb.set("center", vec3_json(f.obb_center));
    obb.set("extents", vec3_json(f.obb_extents));
    obb.set("rotation", quat_json(f.obb_rotation));
    o.set("obb", std::move(obb));
  }
  if (!f.convex.empty()) o.set("convex", hulls_json(f.convex));
  if (!f.pieces.empty()) o.set("pieces", hulls_json(f.pieces));
  return o;
}

void apply_fitted_cache(Model* m, const json::Value& info, const FitParams& params) {
  if (!m) return;
  const json::Value& f = info["fitted"];
  if (!f.is_object()) return;
  const FitParams p = clamp_fit_params(params);
  FittedCollision c;
  try {
    c.fingerprint = static_cast<std::uint64_t>(std::stoull(f.str("fingerprint")));
  } catch (...) {
    return;
  }
  c.vertex_limit = static_cast<int>(f.num("vertex_limit", p.hull_vertices));
  c.decompose_max = static_cast<int>(f.num("decompose_max", p.decompose_max));
  if (c.fingerprint != mesh_fingerprint(*m) || c.vertex_limit != p.hull_vertices ||
      c.decompose_max != p.decompose_max) {
    return;
  }
  const json::Value& obb = f["obb"];
  if (obb.is_object()) {
    float r[4];
    if (read_vec3(obb["center"], &c.obb_center) && read_vec3(obb["extents"], &c.obb_extents) &&
        obb["rotation"].is_array() && obb["rotation"].size() >= 4) {
      c.obb_rotation = {obb["rotation"][0].as_float(), obb["rotation"][1].as_float(),
                        obb["rotation"][2].as_float(), obb["rotation"][3].as_float()};
      c.has_obb = true;
    }
  }
  if (!read_hulls(f["convex"], &c.convex)) return;
  if (!read_hulls(f["pieces"], &c.pieces)) c.pieces = c.convex;
  m->fitted = std::move(c);
}

FitParams fit_params_for_file(const std::string& model_path) {
  if (model_path.empty()) return {};
  const std::string meta = core::asset_meta::find_for_file(model_path);
  std::ifstream in(std::filesystem::u8path(meta.empty() ? model_path + ".meta" : meta), std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  json::Value root;
  std::string err;
  if (!json::parse(ss.str(), &root, &err)) return {};
  return fit_params_from_settings(root["settings"]);
}

void apply_fitted_cache_file(const std::string& model_path, Model* m) {
  if (!m || model_path.empty()) return;
  const std::string meta = core::asset_meta::find_for_file(model_path);
  std::ifstream in(std::filesystem::u8path(meta.empty() ? model_path + ".meta" : meta), std::ios::binary);
  if (!in) return;
  std::ostringstream ss;
  ss << in.rdbuf();
  json::Value root;
  std::string err;
  if (!json::parse(ss.str(), &root, &err)) return;
  apply_fitted_cache(m, root["info"], fit_params_from_settings(root["settings"]));
}

std::vector<Vec3> convex_hull(const std::vector<Vec3>& points, int limit) {
  const HullBuild h = build_hull(limit_cloud(dedup_points(points), limit));
  return h.points;
}

void hull_wire(const std::vector<Vec3>& points, std::vector<std::pair<Vec3, Vec3>>* edges) {
  if (!edges) return;
  edges->clear();
  const HullBuild h = build_hull(points);
  auto key = [](int a, int b) { return a < b ? (static_cast<std::uint64_t>(a) << 32) | b
                                             : (static_cast<std::uint64_t>(b) << 32) | a; };
  std::vector<std::uint64_t> seen;
  for (const Face& f : h.faces) {
    for (int e = 0; e < 3; ++e) {
      const int a = f.v[e], b = f.v[(e + 1) % 3];
      const std::uint64_t k = key(a, b);
      if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
      seen.push_back(k);
      edges->push_back({h.points[a], h.points[b]});
    }
  }
}

bool hull_contains(const std::vector<Vec3>& points, const Vec3& p, float slack) {
  const HullBuild h = build_hull(points);
  if (h.faces.empty()) return false;
  for (const Face& f : h.faces) {
    if (face_dist(f, p) > slack) return false;
  }
  return true;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
