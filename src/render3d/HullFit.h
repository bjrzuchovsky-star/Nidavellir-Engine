#pragma once

// Mesh-fitted colliders (docs/model-import.md, docs/physics.md).
//
//   box        axis-aligned tight box (the old fit)
//   obb        oriented box, used when it is clearly tighter than the
//              axis-aligned one (a crate stays a plain box)
//   convex     one convex hull of the drawn mesh, simplified to a vertex
//              limit
//   decompose  several convex pieces (approximate convex decomposition,
//              the same idea as V-HACD / CoACD, our own code) for concave
//              dynamic props
//   mesh       the model's triangles, for static geometry
//
// The generated hulls are stored in the model's .meta ("fitted") keyed by
// a fingerprint of the drawn vertices and the limits, so a load does not
// rebuild them. A reimport changes the fingerprint and writes a new cache.

#include "render3d/Gltf.h"
#include "scene/JsonTree.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

struct FitParams {
  int hull_vertices = 64;  // 4..256, per convex piece
  int decompose_max = 8;   // 1..32 pieces
  float concavity = 0.06f; // stop merging pieces once the gap is this large
};

FitParams clamp_fit_params(FitParams p);
FitParams fit_params_from_settings(const json::Value& settings);
// Limits stored on the model's .meta, or the defaults when it has none.
FitParams fit_params_for_file(const std::string& model_path);

// FNV-1a of the rest-pose drawn vertices. Changes when the mesh does.
std::uint64_t mesh_fingerprint(const Model& m);

// Build (or keep) model.fitted. False only when the model has no triangles.
bool ensure_fitted(Model* m, const FitParams& params);
// Read info["fitted"] when it still matches this mesh and these limits.
void apply_fitted_cache(Model* m, const json::Value& info, const FitParams& params);
json::Value fitted_to_json(const FittedCollision& f);
// The claim's .meta for this file, when it carries a matching cache.
void apply_fitted_cache_file(const std::string& model_path, Model* m);

// Convex hull of `points`, at most `limit` vertices (0 = no limit).
std::vector<Vec3> convex_hull(const std::vector<Vec3>& points, int limit);
// Unique edges of that hull, for the viewport wireframe.
void hull_wire(const std::vector<Vec3>& points, std::vector<std::pair<Vec3, Vec3>>* edges);
// True when `p` sits inside the hull (a little slack for boundary points).
bool hull_contains(const std::vector<Vec3>& points, const Vec3& p, float slack = 1.0e-3f);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
