#include "runtime/SpritePoly.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {
namespace {

struct P {
  float x = 0.0f, y = 0.0f;
};

float cross(const P& a, const P& b, const P& c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

float area(const std::vector<P>& p) {
  float a = 0.0f;
  for (std::size_t i = 0; i < p.size(); ++i) {
    const P& q = p[i];
    const P& r = p[(i + 1) % p.size()];
    a += q.x * r.y - r.x * q.y;
  }
  return a * 0.5f;
}

bool nearly(const P& a, const P& b) { return std::fabs(a.x - b.x) < 1.0e-3f && std::fabs(a.y - b.y) < 1.0e-3f; }

std::vector<P> simplify(const std::vector<P>& in) {
  std::vector<P> p;
  for (const P& v : in) {
    if (!p.empty() && nearly(p.back(), v)) continue;
    p.push_back(v);
  }
  if (p.size() >= 2 && nearly(p.front(), p.back())) p.pop_back();
  // Drop colinear corners.
  bool changed = true;
  while (changed && p.size() >= 3) {
    changed = false;
    for (std::size_t i = 0; i < p.size();) {
      const P& a = p[(i + p.size() - 1) % p.size()];
      const P& b = p[i];
      const P& c = p[(i + 1) % p.size()];
      if (std::fabs(cross(a, b, c)) < 1.0e-3f) {
        p.erase(p.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
      } else {
        ++i;
      }
    }
  }
  return p;
}

bool convex_poly(const std::vector<P>& p) {
  if (p.size() < 3) return false;
  const bool ccw = area(p) >= 0.0f;
  for (std::size_t i = 0; i < p.size(); ++i) {
    const float c = cross(p[i], p[(i + 1) % p.size()], p[(i + 2) % p.size()]);
    if (ccw ? c < -1.0e-3f : c > 1.0e-3f) return false;
  }
  return true;
}

bool point_in_tri(const P& a, const P& b, const P& c, const P& p) {
  const float c1 = cross(a, b, p), c2 = cross(b, c, p), c3 = cross(c, a, p);
  const bool neg = c1 < -1.0e-4f || c2 < -1.0e-4f || c3 < -1.0e-4f;
  const bool pos = c1 > 1.0e-4f || c2 > 1.0e-4f || c3 > 1.0e-4f;
  return !(neg && pos);
}

// Ear clipping. `poly` is simple. Returns triangles as index triples into a copy.
std::vector<std::vector<P>> triangulate(std::vector<P> poly) {
  std::vector<std::vector<P>> tris;
  if (area(poly) < 0.0f) std::reverse(poly.begin(), poly.end());
  std::vector<int> idx(poly.size());
  for (int i = 0; i < static_cast<int>(poly.size()); ++i) idx[i] = i;
  int guard = static_cast<int>(poly.size()) * static_cast<int>(poly.size());
  while (idx.size() > 3 && guard-- > 0) {
    bool clipped = false;
    for (std::size_t i = 0; i < idx.size(); ++i) {
      const int ip = idx[(i + idx.size() - 1) % idx.size()];
      const int ic = idx[i];
      const int in = idx[(i + 1) % idx.size()];
      if (cross(poly[ip], poly[ic], poly[in]) <= 1.0e-4f) continue;
      bool ear = true;
      for (int j : idx) {
        if (j == ip || j == ic || j == in) continue;
        if (point_in_tri(poly[ip], poly[ic], poly[in], poly[j])) {
          ear = false;
          break;
        }
      }
      if (!ear) continue;
      tris.push_back({poly[ip], poly[ic], poly[in]});
      idx.erase(idx.begin() + static_cast<std::ptrdiff_t>(i));
      clipped = true;
      break;
    }
    if (!clipped) break;
  }
  if (idx.size() == 3) tris.push_back({poly[idx[0]], poly[idx[1]], poly[idx[2]]});
  return tris;
}

// Stitch two polygons that share an edge walked in opposite directions.
// The shared edge drops out; the outlines join into one loop.
std::vector<P> merge_polys(const std::vector<P>& a, const std::vector<P>& b) {
  for (std::size_t i = 0; i < a.size(); ++i) {
    const P& a0 = a[i];
    const P& a1 = a[(i + 1) % a.size()];
    for (std::size_t j = 0; j < b.size(); ++j) {
      const P& b0 = b[j];
      const P& b1 = b[(j + 1) % b.size()];
      if (!(nearly(a0, b1) && nearly(a1, b0))) continue;
      std::vector<P> merged;
      for (std::size_t k = 0; k < a.size(); ++k) merged.push_back(a[(i + 1 + k) % a.size()]);
      for (std::size_t k = 1; k + 1 < b.size(); ++k) merged.push_back(b[(j + 1 + k) % b.size()]);
      return simplify(merged);
    }
  }
  return {};
}

std::vector<std::vector<P>> convex_pieces(std::vector<P> outline, int max_verts, int max_pieces) {
  outline = simplify(outline);
  if (outline.size() < 3) return {};
  if (convex_poly(outline) && static_cast<int>(outline.size()) <= max_verts) return {outline};
  if (static_cast<int>(outline.size()) > max_verts && convex_poly(outline)) {
    // A convex outline with too many corners: keep the most spread out.
    while (static_cast<int>(outline.size()) > max_verts) {
      float best = 1.0e30f;
      std::size_t drop = 0;
      for (std::size_t i = 0; i < outline.size(); ++i) {
        const float c = std::fabs(cross(outline[(i + outline.size() - 1) % outline.size()], outline[i],
                                        outline[(i + 1) % outline.size()]));
        if (c < best) {
          best = c;
          drop = i;
        }
      }
      outline.erase(outline.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    return {outline};
  }
  std::vector<std::vector<P>> tris = triangulate(outline);
  if (tris.empty()) {
    // Fall back to the convex hull of the outline so the collider still exists.
    std::vector<P> hull = outline;
    std::sort(hull.begin(), hull.end(), [](const P& a, const P& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    std::vector<P> lower, upper;
    for (const P& p : hull) {
      while (lower.size() >= 2 && cross(lower[lower.size() - 2], lower.back(), p) <= 0) lower.pop_back();
      lower.push_back(p);
    }
    for (int i = static_cast<int>(hull.size()) - 1; i >= 0; --i) {
      const P& p = hull[i];
      while (upper.size() >= 2 && cross(upper[upper.size() - 2], upper.back(), p) <= 0) upper.pop_back();
      upper.push_back(p);
    }
    lower.pop_back();
    upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return convex_pieces(lower, max_verts, 1);
  }
  // Greedy merge of triangles that share an edge while the result stays
  // convex and under the vertex cap.
  std::vector<char> alive(tris.size(), 1);
  bool merged = true;
  while (merged) {
    merged = false;
    for (std::size_t i = 0; i < tris.size() && !merged; ++i) {
      if (!alive[i]) continue;
      for (std::size_t j = i + 1; j < tris.size(); ++j) {
        if (!alive[j]) continue;
        std::vector<P> m = merge_polys(tris[i], tris[j]);
        if (m.size() < 3 || static_cast<int>(m.size()) > max_verts) continue;
        if (!convex_poly(m)) continue;
        tris[i] = std::move(m);
        alive[j] = 0;
        merged = true;
        break;
      }
    }
  }
  std::vector<std::vector<P>> pieces;
  for (std::size_t i = 0; i < tris.size(); ++i)
    if (alive[i]) pieces.push_back(tris[i]);
  if (static_cast<int>(pieces.size()) > max_pieces) pieces.resize(max_pieces);
  return pieces;
}

struct Edge {
  int x0, y0, x1, y1;
};

std::vector<P> longest_loop(const unsigned char* rgba, int w, int h, int threshold) {
  auto solid = [&](int x, int y) {
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    return rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 3] >= threshold;
  };
  std::vector<Edge> edges;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      if (!solid(x, y)) continue;
      // Corners of pixel (x, y): (x,y) (x+1,y) (x+1,y+1) (x,y+1).
      // Direct the edge so the solid pixel is on the left of travel? We only
      // need a chainable direction: one outgoing edge per boundary side.
      if (!solid(x - 1, y)) edges.push_back({x, y + 1, x, y});          // left, upward
      if (!solid(x + 1, y)) edges.push_back({x + 1, y, x + 1, y + 1});  // right, downward
      if (!solid(x, y - 1)) edges.push_back({x, y, x + 1, y});          // top, rightward
      if (!solid(x, y + 1)) edges.push_back({x + 1, y + 1, x, y + 1});  // bottom, leftward
    }
  }
  std::map<std::pair<int, int>, std::vector<int>> from;
  for (int i = 0; i < static_cast<int>(edges.size()); ++i) from[{edges[i].x0, edges[i].y0}].push_back(i);
  std::vector<char> used(edges.size(), 0);
  std::vector<P> best;
  float best_area = 0.0f;
  for (int start = 0; start < static_cast<int>(edges.size()); ++start) {
    if (used[start]) continue;
    std::vector<P> loop;
    int cur = start;
    for (int guard = 0; guard < static_cast<int>(edges.size()) + 2; ++guard) {
      if (used[cur]) break;
      used[cur] = 1;
      const Edge& e = edges[cur];
      loop.push_back({static_cast<float>(e.x0), static_cast<float>(e.y0)});
      const auto it = from.find({e.x1, e.y1});
      if (it == from.end()) break;
      int next = -1;
      for (int n : it->second) {
        if (!used[n]) {
          next = n;
          break;
        }
      }
      if (next < 0) {
        // Closed if some edge returns to the start vertex.
        if (e.x1 == edges[start].x0 && e.y1 == edges[start].y0) {
          const float a = std::fabs(area(loop));
          if (a > best_area) {
            best_area = a;
            best = loop;
          }
        }
        break;
      }
      cur = next;
    }
  }
  return best;
}

std::vector<std::pair<float, float>> as_pairs(const std::vector<P>& p) {
  std::vector<std::pair<float, float>> o;
  o.reserve(p.size());
  for (const P& v : p) o.emplace_back(v.x, v.y);
  return o;
}

}  // namespace

std::vector<std::vector<std::pair<float, float>>> trace_sprite_polygon(const unsigned char* rgba, int width,
                                                                       int height, const SpritePolyOptions& opt) {
  std::vector<std::vector<std::pair<float, float>>> out;
  if (!rgba || width <= 0 || height <= 0) return out;
  SpritePolyOptions o = opt;
  o.alpha_threshold = std::clamp(o.alpha_threshold, 0, 255);
  o.max_vertices = std::clamp(o.max_vertices, 3, 8);
  o.max_pieces = std::clamp(o.max_pieces, 1, 16);
  const std::vector<P> loop = longest_loop(rgba, width, height, o.alpha_threshold);
  if (loop.size() < 3) return out;
  for (const std::vector<P>& piece : convex_pieces(loop, o.max_vertices, o.max_pieces)) {
    if (piece.size() >= 3) out.push_back(as_pairs(piece));
  }
  return out;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
