#include "render3d/ColliderFit.h"
#include "render3d/ModelImport.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

const char* collider_fit_name(ColliderFit f) {
  switch (f) {
    case ColliderFit::Auto: return "auto";
    case ColliderFit::Box: return "box";
    case ColliderFit::Obb: return "obb";
    case ColliderFit::Capsule: return "capsule";
    case ColliderFit::Convex: return "convex";
    case ColliderFit::Decompose: return "decompose";
    case ColliderFit::Mesh: return "mesh";
    case ColliderFit::None: return "none";
  }
  return "auto";
}

ColliderFit collider_fit_from(const std::string& n) {
  if (n == "box") return ColliderFit::Box;
  if (n == "obb") return ColliderFit::Obb;
  if (n == "capsule") return ColliderFit::Capsule;
  if (n == "convex") return ColliderFit::Convex;
  if (n == "decompose") return ColliderFit::Decompose;
  if (n == "mesh") return ColliderFit::Mesh;
  if (n == "none") return ColliderFit::None;
  return ColliderFit::Auto;
}

ColliderFit resolve_collider_fit(const Model& m, ColliderFit f) {
  if (f != ColliderFit::Auto) return f;
  if (!m.collision_hulls.empty()) return ColliderFit::Convex;
  if (m.skinned()) return ColliderFit::Capsule;
  return ColliderFit::Obb;
}

namespace {

// An oriented box is only worth it when it is clearly tighter than the
// axis-aligned one. A cube's principal axes are a coin toss, and that
// coin toss must not turn a crate into a rotated box of the same size.
bool obb_is_tighter(const Aabb& box, const FittedCollision& f) {
  if (!f.has_obb) return false;
  const Vec3 e = box.extent();
  const float aabb = std::max(1.0e-6f, e.x * e.y * e.z);
  const float obb = f.obb_extents.x * f.obb_extents.y * f.obb_extents.z;
  return obb < aabb * 0.92f;
}

}  // namespace

bool fit_collider(const Model& m, ColliderFit f, scene3d::Collider3D* out, const FitParams& params) {
  const ColliderFit r = resolve_collider_fit(m, f);
  if (r == ColliderFit::None || !out) return false;
  FittedCollision fitted = m.fitted;
  if (!fitted.matches(mesh_fingerprint(m), clamp_fit_params(params).hull_vertices,
                      clamp_fit_params(params).decompose_max)) {
    Model copy = m;
    if (ensure_fitted(&copy, params)) fitted = copy.fitted;
  }
  Aabb b = m.bounds;
  // Flat models (planes, decals): a sliver so the box still collides.
  auto sliver = [](float& lo, float& hi) {
    if (hi - lo < 0.02f) {
      const float c = 0.5f * (lo + hi);
      lo = c - 0.01f;
      hi = c + 0.01f;
    }
  };
  sliver(b.min.x, b.max.x);
  sliver(b.min.y, b.max.y);
  sliver(b.min.z, b.max.z);
  const Vec3 ext = b.extent(), ctr = b.center();
  out->fit = collider_fit_name(f);
  out->rotation_deg = {};
  switch (r) {
    case ColliderFit::Box:
      out->shape = scene3d::ColliderShape3D::Box;
      out->size = ext;
      out->offset = ctr;
      break;
    case ColliderFit::Obb:
      if (obb_is_tighter(b, fitted)) {
        out->shape = scene3d::ColliderShape3D::Box;
        out->size = fitted.obb_extents;
        out->offset = fitted.obb_center;
        out->rotation_deg = fitted.obb_rotation.to_euler_deg();
      } else {
        out->shape = scene3d::ColliderShape3D::Box;
        out->size = ext;
        out->offset = ctr;
      }
      break;
    case ColliderFit::Capsule: {
      // Upright capsule: the bind pose's height; the radius from where
      // most vertices are (80th percentile of the distance from the
      // vertical axis) so T-pose arms do not make it a barrel.
      MeshData rest;
      rest_pose_mesh(m, &rest);
      std::vector<float> d;
      d.reserve(rest.indices.size());
      for (std::uint32_t i : rest.indices) {
        if (i >= rest.vertices.size()) continue;
        const Vec3& p = rest.vertices[i].position;
        d.push_back(std::sqrt((p.x - ctr.x) * (p.x - ctr.x) + (p.z - ctr.z) * (p.z - ctr.z)));
      }
      float radius = 0.5f * std::min(ext.x, ext.z);
      if (!d.empty()) {
        const std::size_t k = std::min(d.size() - 1, d.size() * 8 / 10);
        std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(k), d.end());
        radius = std::max(radius, d[k]);
      }
      radius = std::min({radius, 0.5f * std::max(ext.x, ext.z), 0.5f * ext.y});
      radius = std::max(radius, 0.01f);
      out->shape = scene3d::ColliderShape3D::Capsule;
      out->size = {radius, std::max(ext.y, 2.0f * radius), radius};
      out->offset = ctr;
      break;
    }
    case ColliderFit::Convex:
    case ColliderFit::Decompose:
      out->shape = scene3d::ColliderShape3D::ConvexHull;
      out->size = ext;  // unused by the hull; the box the editor shows
      out->offset = {};
      break;
    case ColliderFit::Mesh:
      out->shape = scene3d::ColliderShape3D::Mesh;
      out->size = ext;
      out->offset = {};
      break;
    default: break;
  }
  return true;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
