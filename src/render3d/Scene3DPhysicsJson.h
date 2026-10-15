#pragma once

// scene.json v14 physics keys (collider, rigid_body, joint, "physics"),
// kept apart from Scene3D.cpp. Internal to ts_render3d.

#include "render3d/Json.h"
#include "render3d/Scene3D.h"

namespace ts {
namespace tombstone {
namespace scene3d {
namespace physics_json {

json::Value collider(const Collider3D& c);
Collider3D read_collider(const json::Value& v);
json::Value rigid_body(const RigidBody3D& b);
RigidBody3D read_rigid_body(const json::Value& v);
json::Value joint(const Joint3D& j);
Joint3D read_joint(const json::Value& v);
// Null when everything is at its default (older files stay unchanged).
json::Value settings(const PhysicsSettings3D& s);
PhysicsSettings3D read_settings(const json::Value& v);

}  // namespace physics_json
}  // namespace scene3d
}  // namespace tombstone
}  // namespace ts
