#pragma once

// Colliders fitted to imported models (render3d/ModelImport.h). The
// Asset Browser's "collider" import setting and the inspector's
// "Fit to mesh" button both come here, so a placed model and a refit agree.
//
//   auto     convex hulls when the model brings its own (Source .phy,
//            UCX_ meshes), a capsule for skinned (character) models, else
//            a tight box
//   box      tight box around the drawn vertices (Model::bounds)
//   capsule  upright capsule fitted to the rest-pose (bind) vertices
//   convex   convex hull(s) of the model
//   mesh     the model's triangles (static bodies)
//   none     no collider

#include "render3d/Gltf.h"
#include "render3d/Scene3D.h"

#include <string>

namespace ts {
namespace tombstone {
namespace render3d {

enum class ColliderFit : std::uint8_t { Auto, Box, Capsule, Convex, Mesh, None };

const char* collider_fit_name(ColliderFit f);      // "auto", "box"...
ColliderFit collider_fit_from(const std::string& name);  // unknown -> Auto
// What Auto picks for this model.
ColliderFit resolve_collider_fit(const Model& m, ColliderFit f);
// Fill *out (shape, size, offset; fit = the setting's name). False for
// None (no collider wanted). Trigger / layer / parts are left alone.
bool fit_collider(const Model& m, ColliderFit f, scene3d::Collider3D* out);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
