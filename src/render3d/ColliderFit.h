#pragma once

// Colliders fitted to imported models (render3d/ModelImport.h). The
// Asset Browser's "collider" import setting and the inspector's
// "Fit to mesh" button both come here, so a placed model and a refit agree.
//
//   auto       the model's own hulls when it has them, a capsule for a
//              skinned model, else an oriented box when that is tighter
//              than the axis-aligned box (a crate stays a plain box)
//   box        axis-aligned tight box (Model::bounds)
//   obb        oriented tight box
//   capsule    upright capsule fitted to the rest-pose vertices
//   convex     one convex hull of the drawn mesh (vertex limit in FitParams)
//   decompose  several convex pieces for a concave dynamic prop
//   mesh       the model's triangles (static bodies)
//   none       no collider

#include "render3d/Gltf.h"
#include "render3d/HullFit.h"
#include "render3d/Scene3D.h"

#include <string>

namespace ts {
namespace tombstone {
namespace render3d {

enum class ColliderFit : std::uint8_t { Auto, Box, Obb, Capsule, Convex, Decompose, Mesh, None };

const char* collider_fit_name(ColliderFit f);      // "auto", "box"...
ColliderFit collider_fit_from(const std::string& name);  // unknown -> Auto
// What Auto picks for this model.
ColliderFit resolve_collider_fit(const Model& m, ColliderFit f);
// Fill *out (shape, size, offset, rotation; fit = the setting's name).
// False for None. Trigger / layer / parts are left alone. Convex pieces
// live on the model (Model::fitted), not on the collider.
bool fit_collider(const Model& m, ColliderFit f, scene3d::Collider3D* out,
                  const FitParams& params = {});

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
