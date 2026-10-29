#include "render3d/Scene3DPhysicsJson.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace scene3d {

using json::Value;

const char* collider_shape_name(ColliderShape3D s) {
  switch (s) {
    case ColliderShape3D::Box: return "box";
    case ColliderShape3D::Capsule: return "capsule";
    case ColliderShape3D::Sphere: return "sphere";
    case ColliderShape3D::ConvexHull: return "convex";
    case ColliderShape3D::Mesh: return "mesh";
  }
  return "box";
}

ColliderShape3D collider_shape_from(const std::string& n) {
  if (n == "capsule") return ColliderShape3D::Capsule;
  if (n == "sphere") return ColliderShape3D::Sphere;
  if (n == "convex") return ColliderShape3D::ConvexHull;
  if (n == "mesh") return ColliderShape3D::Mesh;
  return ColliderShape3D::Box;
}

const char* motion_name(RigidBody3D::Motion m) {
  switch (m) {
    case RigidBody3D::Motion::Static: return "static";
    case RigidBody3D::Motion::Kinematic: return "kinematic";
    case RigidBody3D::Motion::Dynamic: return "dynamic";
  }
  return "dynamic";
}

const char* joint_type_name(Joint3D::Type t) {
  switch (t) {
    case Joint3D::Type::Fixed: return "fixed";
    case Joint3D::Type::Hinge: return "hinge";
    case Joint3D::Type::Slider: return "slider";
    case Joint3D::Type::Distance: return "distance";
    case Joint3D::Type::Ball: return "ball";
  }
  return "hinge";
}

namespace physics_json {
namespace {

Value vec3(const Vec3& v) {
  const float f[3] = {v.x, v.y, v.z};
  return json::floats(f, 3);
}
void read_vec3(const Value& o, const std::string& key, Vec3* v) {
  float f[3];
  if (o.floats(key, f, 3)) *v = {f[0], f[1], f[2]};
}
float finite(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

}  // namespace

Value collider(const Collider3D& c) {
  Value o = Value::object();
  o.set("shape", Value::string(collider_shape_name(c.shape)));
  o.set("size", vec3(c.size));
  o.set("offset", vec3(c.offset));
  if (!(c.rotation_deg == Vec3{})) o.set("rotation", vec3(c.rotation_deg));
  if (c.trigger) o.set("trigger", Value::boolean(true));
  if (c.layer >= 0) o.set("layer", Value::number(c.layer));
  if (!c.fit.empty()) o.set("fit", Value::string(c.fit));
  if (!c.parts.empty()) {
    Value parts = Value::array();
    for (const ColliderPart3D& p : c.parts) {
      Value q = Value::object();
      q.set("shape", Value::string(collider_shape_name(p.shape)));
      q.set("size", vec3(p.size));
      q.set("offset", vec3(p.offset));
      if (!(p.rotation_deg == Vec3{})) q.set("rotation", vec3(p.rotation_deg));
      parts.push(std::move(q));
    }
    o.set("parts", std::move(parts));
  }
  return o;
}

Collider3D read_collider(const Value& v) {
  Collider3D c;
  c.shape = collider_shape_from(v.str("shape"));
  read_vec3(v, "size", &c.size);
  read_vec3(v, "offset", &c.offset);
  read_vec3(v, "rotation", &c.rotation_deg);
  c.trigger = v.flag("trigger", false);
  c.layer = std::clamp(static_cast<int>(v.num("layer", -1)), -1, 7);
  c.fit = v.str("fit");
  for (const Value& q : v["parts"].items()) {
    if (!q.is_object() || c.parts.size() >= 32) continue;
    ColliderPart3D p;
    p.shape = collider_shape_from(q.str("shape"));
    // Parts are primitives (a hull / mesh part makes no sense).
    if (p.shape == ColliderShape3D::ConvexHull || p.shape == ColliderShape3D::Mesh) {
      p.shape = ColliderShape3D::Box;
    }
    read_vec3(q, "size", &p.size);
    read_vec3(q, "offset", &p.offset);
    read_vec3(q, "rotation", &p.rotation_deg);
    c.parts.push_back(p);
  }
  return c;
}

Value rigid_body(const RigidBody3D& b) {
  Value o = Value::object();
  o.set("motion", Value::string(motion_name(b.motion)));
  o.set("mass", Value::number(b.mass));
  o.set("friction", Value::number(b.friction));
  o.set("restitution", Value::number(b.restitution));
  o.set("linear_damping", Value::number(b.linear_damping));
  o.set("angular_damping", Value::number(b.angular_damping));
  if (b.gravity_scale != 1.0f) o.set("gravity_scale", Value::number(b.gravity_scale));
  if (b.ccd) o.set("ccd", Value::boolean(true));
  if (!b.impact_events) o.set("impact_events", Value::boolean(false));
  return o;
}

RigidBody3D read_rigid_body(const Value& v) {
  RigidBody3D b;
  const std::string m = v.str("motion");
  b.motion = m == "static"      ? RigidBody3D::Motion::Static
             : m == "kinematic" ? RigidBody3D::Motion::Kinematic
                                : RigidBody3D::Motion::Dynamic;
  b.mass = std::clamp(finite(v.flt("mass", b.mass), 0.0f), 0.0f, 100000.0f);
  b.friction = std::clamp(finite(v.flt("friction", b.friction), 0.5f), 0.0f, 10.0f);
  b.restitution = std::clamp(finite(v.flt("restitution", b.restitution), 0.1f), 0.0f, 1.0f);
  b.linear_damping = std::clamp(finite(v.flt("linear_damping", b.linear_damping), 0.05f), 0.0f, 100.0f);
  b.angular_damping = std::clamp(finite(v.flt("angular_damping", b.angular_damping), 0.05f), 0.0f, 100.0f);
  b.gravity_scale = std::clamp(finite(v.flt("gravity_scale", b.gravity_scale), 1.0f), -10.0f, 10.0f);
  b.ccd = v.flag("ccd", false);
  b.impact_events = v.flag("impact_events", true);
  return b;
}

Value joint(const Joint3D& j) {
  Value o = Value::object();
  o.set("type", Value::string(joint_type_name(j.type)));
  o.set("target", Value::number(static_cast<double>(j.target)));
  o.set("anchor", vec3(j.anchor));
  o.set("axis", vec3(j.axis));
  if (j.limits) {
    o.set("limits", Value::boolean(true));
    o.set("min", Value::number(j.min));
    o.set("max", Value::number(j.max));
  }
  return o;
}

Joint3D read_joint(const Value& v) {
  Joint3D j;
  const std::string t = v.str("type");
  j.type = t == "fixed"      ? Joint3D::Type::Fixed
           : t == "slider"   ? Joint3D::Type::Slider
           : t == "distance" ? Joint3D::Type::Distance
           : t == "ball"     ? Joint3D::Type::Ball
                             : Joint3D::Type::Hinge;
  const double target = v.num("target", 0);
  j.target = target > 0 ? static_cast<std::uint64_t>(target + 0.5) : 0;
  read_vec3(v, "anchor", &j.anchor);
  read_vec3(v, "axis", &j.axis);
  if (render3d::length(j.axis) < 1e-4f) j.axis = {0, 1, 0};
  j.limits = v.flag("limits", false);
  j.min = finite(v.flt("min", 0.0f), 0.0f);
  j.max = finite(v.flt("max", 0.0f), 0.0f);
  return j;
}

Value settings(const PhysicsSettings3D& s) {
  if (s == PhysicsSettings3D{}) return Value::null();
  const PhysicsSettings3D d;
  Value o = Value::object();
  o.set("gravity", vec3(s.gravity));
  o.set("collision_steps", Value::number(s.collision_steps));
  Value names = Value::array();
  Value matrix = Value::array();
  for (int i = 0; i < 8; ++i) {
    names.push(Value::string(s.layer_names[i]));
    matrix.push(Value::number(s.collide[i]));
  }
  o.set("layer_names", std::move(names));
  o.set("collide", std::move(matrix));
  if (s.debug_draw) o.set("debug_draw", Value::boolean(true));
  return o;
}

PhysicsSettings3D read_settings(const Value& v) {
  PhysicsSettings3D s;
  if (!v.is_object()) return s;
  read_vec3(v, "gravity", &s.gravity);
  if (!std::isfinite(s.gravity.x) || !std::isfinite(s.gravity.y) || !std::isfinite(s.gravity.z)) {
    s.gravity = {0.0f, -9.81f, 0.0f};
  }
  s.collision_steps = std::clamp(static_cast<int>(v.num("collision_steps", 1)), 1, 8);
  const Value& names = v["layer_names"];
  for (std::size_t i = 0; i < 8 && i < names.size(); ++i) {
    const std::string n = names[i].as_string_or("");
    if (!n.empty()) s.layer_names[i] = n.substr(0, 32);
  }
  const Value& m = v["collide"];
  if (m.is_array() && m.size() == 8) {
    for (int i = 0; i < 8; ++i) {
      s.collide[i] = static_cast<std::uint8_t>(std::clamp(m[i].as_number(0xff), 0.0, 255.0));
    }
    // Keep it symmetric (a bit set on either side wins).
    for (int i = 0; i < 8; ++i) {
      for (int j = 0; j < 8; ++j) {
        if (((s.collide[i] >> j) & 1u) || ((s.collide[j] >> i) & 1u)) {
          s.collide[i] |= static_cast<std::uint8_t>(1u << j);
          s.collide[j] |= static_cast<std::uint8_t>(1u << i);
        }
      }
    }
  }
  s.debug_draw = v.flag("debug_draw", false);
  return s;
}

}  // namespace physics_json
}  // namespace scene3d
}  // namespace tombstone
}  // namespace ts
