#include "render3d/Mesh.h"

#include <cmath>

namespace ts {
namespace tombstone {
namespace render3d {

Aabb MeshData::bounds() const {
  if (vertices.empty()) return {};
  Aabb b{vertices[0].position, vertices[0].position};
  for (const Vertex3D& v : vertices) {
    b.min = vmin(b.min, v.position);
    b.max = vmax(b.max, v.position);
  }
  return b;
}

MeshData make_cube(float size) {
  MeshData m;
  m.name = "cube";
  const float h = size * 0.5f;
  const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                           {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (const Vec3& n : normals) {
    // a x b = n, so (a, b) wind counter-clockwise seen from outside.
    const Vec3 a = std::fabs(n.y) > 0.5f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const Vec3 b = cross(n, a) * -1.0f;
    // b x a == n  ->  quad (-a-b, +b-a...) built so the winding is CCW.
    const auto base = static_cast<std::uint32_t>(m.vertices.size());
    const float sa[4] = {-1, -1, 1, 1};
    const float sb[4] = {-1, 1, 1, -1};
    for (int i = 0; i < 4; ++i) {
      Vertex3D v;
      v.position = (n + a * sa[i] + b * sb[i]) * h;
      v.normal = n;
      v.u = (sb[i] + 1.0f) * 0.5f;
      v.v = (1.0f - sa[i]) * 0.5f;
      m.vertices.push_back(v);
    }
    // Pick the winding whose face normal matches n.
    const Vec3 p0 = m.vertices[base].position, p1 = m.vertices[base + 1].position,
               p2 = m.vertices[base + 2].position;
    const bool ccw = dot(cross(p1 - p0, p2 - p0), n) > 0.0f;
    const std::uint32_t fwd[6] = {0, 1, 2, 0, 2, 3};
    const std::uint32_t rev[6] = {0, 2, 1, 0, 3, 2};
    for (std::uint32_t i : (ccw ? fwd : rev)) m.indices.push_back(base + i);
  }
  compute_tangents(m);
  return m;
}

MeshData make_plane(float size, int divisions) {
  MeshData m;
  m.name = "plane";
  const int n = divisions < 1 ? 1 : divisions;
  const float h = size * 0.5f;
  for (int z = 0; z <= n; ++z) {
    for (int x = 0; x <= n; ++x) {
      Vertex3D v;
      const float fx = static_cast<float>(x) / n, fz = static_cast<float>(z) / n;
      v.position = {-h + size * fx, 0.0f, -h + size * fz};
      v.normal = {0, 1, 0};
      v.u = fx;
      v.v = fz;
      m.vertices.push_back(v);
    }
  }
  const auto row = static_cast<std::uint32_t>(n + 1);
  for (int z = 0; z < n; ++z) {
    for (int x = 0; x < n; ++x) {
      const std::uint32_t i0 = static_cast<std::uint32_t>(z) * row + x;
      const std::uint32_t i1 = i0 + 1, i2 = i0 + row, i3 = i2 + 1;
      // CCW seen from +Y.
      m.indices.insert(m.indices.end(), {i0, i2, i1, i1, i2, i3});
    }
  }
  compute_tangents(m);
  return m;
}

MeshData make_sphere(float diameter, int segments, int rings) {
  MeshData m;
  m.name = "sphere";
  const int seg = segments < 3 ? 3 : segments;
  const int rng = rings < 2 ? 2 : rings;
  const float r = diameter * 0.5f;
  for (int y = 0; y <= rng; ++y) {
    const float v = static_cast<float>(y) / rng;
    const float phi = v * kPi;
    for (int x = 0; x <= seg; ++x) {
      const float u = static_cast<float>(x) / seg;
      const float th = u * 2.0f * kPi;
      Vertex3D vert;
      vert.normal = {std::sin(phi) * std::sin(th), std::cos(phi),
                     std::sin(phi) * std::cos(th)};
      vert.position = vert.normal * r;
      vert.u = u;
      vert.v = v;
      m.vertices.push_back(vert);
    }
  }
  const auto row = static_cast<std::uint32_t>(seg + 1);
  for (int y = 0; y < rng; ++y) {
    for (int x = 0; x < seg; ++x) {
      const std::uint32_t i0 = static_cast<std::uint32_t>(y) * row + x;
      const std::uint32_t i1 = i0 + 1, i2 = i0 + row, i3 = i2 + 1;
      m.indices.insert(m.indices.end(), {i0, i2, i1, i1, i2, i3});
    }
  }
  compute_tangents(m);
  return m;
}

MeshData make_capsule(float height, float radius, int segments) {
  // A sphere split at the equator with the halves pushed apart.
  MeshData m = make_sphere(radius * 2.0f, segments, segments / 2 * 2);
  m.name = "capsule";
  const float half = height * 0.5f - radius;
  for (Vertex3D& v : m.vertices) {
    v.position.y += v.position.y >= 0.0f ? (half > 0 ? half : 0) : -(half > 0 ? half : 0);
  }
  return m;
}

const std::vector<std::string>& primitive_names() {
  static const std::vector<std::string> names = {"cube", "plane", "sphere",
                                                 "capsule"};
  return names;
}

bool make_primitive(const std::string& name0, MeshData* out) {
  std::string name = name0;
  const std::string prefix = "primitive:";
  if (name.rfind(prefix, 0) == 0) name = name.substr(prefix.size());
  if (name == "cube") *out = make_cube(1.0f);
  else if (name == "plane") *out = make_plane(1.0f, 4);
  else if (name == "sphere") *out = make_sphere(1.0f, 32, 16);
  else if (name == "capsule") *out = make_capsule(2.0f, 0.4f, 24);
  else return false;
  return true;
}

void compute_tangents(MeshData& m) {
  const std::size_t n = m.vertices.size();
  std::vector<Vec3> tan(n), bit(n);
  for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    const std::uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
    if (a >= n || b >= n || c >= n) continue;
    const Vertex3D &v0 = m.vertices[a], &v1 = m.vertices[b], &v2 = m.vertices[c];
    const Vec3 e1 = v1.position - v0.position, e2 = v2.position - v0.position;
    const float du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
    const float du2 = v2.u - v0.u, dv2 = v2.v - v0.v;
    const float det = du1 * dv2 - du2 * dv1;
    if (std::fabs(det) < 1e-12f) continue;
    const float r = 1.0f / det;
    const Vec3 t = (e1 * dv2 - e2 * dv1) * r;
    const Vec3 bt = (e2 * du1 - e1 * du2) * r;
    for (std::uint32_t k : {a, b, c}) {
      tan[k] += t;
      bit[k] += bt;
    }
  }
  for (std::size_t i = 0; i < n; ++i) {
    const Vec3 nn = m.vertices[i].normal;
    Vec3 t = tan[i] - nn * dot(nn, tan[i]);
    if (length(t) < 1e-8f) {
      // Any vector perpendicular to the normal.
      t = std::fabs(nn.x) < 0.9f ? cross(nn, Vec3{1, 0, 0}) : cross(nn, Vec3{0, 1, 0});
      t = cross(t, nn);
    }
    t = normalize(t);
    const float w = dot(cross(nn, t), bit[i]) < 0.0f ? -1.0f : 1.0f;
    m.vertices[i].tangent = {t.x, t.y, t.z, w};
  }
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
