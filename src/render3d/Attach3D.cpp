#include "render3d/Attach3D.h"

#include "render3d/Gltf.h"
#include "render3d/Skeleton.h"

#include <cmath>

namespace ts {
namespace tombstone {
namespace scene3d {

using render3d::cross;
using render3d::dot;
using render3d::length;


Quat quat_from_basis(const Vec3& x, const Vec3& y, const Vec3& z) {
  const float m00 = x.x, m10 = x.y, m20 = x.z;
  const float m01 = y.x, m11 = y.y, m21 = y.z;
  const float m02 = z.x, m12 = z.y, m22 = z.z;
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
  return render3d::normalize(q);
}

Quat look_rotation(const Vec3& forward, const Vec3& up) {
  if (length(forward) < 1e-6f) return Quat{};
  const Vec3 f = render3d::normalize(forward);
  Vec3 u = up;
  if (std::fabs(dot(f, render3d::normalize(u))) > 0.999f) u = {0.0f, 0.0f, 1.0f};
  const Vec3 z = f * -1.0f;  // -Z looks forward
  const Vec3 x = render3d::normalize(cross(u, z));
  const Vec3 y = cross(z, x);
  return quat_from_basis(x, y, z);
}

Quat rotation_between(const Vec3& a, const Vec3& b) {
  const Vec3 na = render3d::normalize(a), nb = render3d::normalize(b);
  const float d = dot(na, nb);
  if (d > 0.99999f) return Quat{};
  if (d < -0.99999f) {
    Vec3 axis = cross({1.0f, 0.0f, 0.0f}, na);
    if (length(axis) < 1e-4f) axis = cross({0.0f, 0.0f, 1.0f}, na);
    return Quat::axis_angle(axis, render3d::kPi);
  }
  const Vec3 c = cross(na, nb);
  return render3d::normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

Transform decompose(const Mat4& m) {
  Transform t;
  t.position = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
  Vec3 x{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
  Vec3 y{m.at(0, 1), m.at(1, 1), m.at(2, 1)};
  Vec3 z{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
  t.scale = {length(x), length(y), length(z)};
  if (dot(cross(x, y), z) < 0.0f) t.scale.x = -t.scale.x;  // mirrored
  auto safe = [](const Vec3& v, float s) { return std::fabs(s) > 1e-8f ? v * (1.0f / s) : v; };
  x = safe(x, t.scale.x);
  y = safe(y, t.scale.y);
  z = safe(z, t.scale.z);
  t.rotation = quat_from_basis(x, y, z);
  return t;
}

Entity3D* find(std::vector<Entity3D>& entities, std::uint64_t id) {
  for (Entity3D& e : entities)
    if (e.id == id) return &e;
  return nullptr;
}
const Entity3D* find(const std::vector<Entity3D>& entities, std::uint64_t id) {
  for (const Entity3D& e : entities)
    if (e.id == id) return &e;
  return nullptr;
}

bool would_loop(const std::vector<Entity3D>& entities, std::uint64_t child, std::uint64_t target) {
  if (child == target) return true;
  std::uint64_t cur = target;
  for (std::size_t steps = 0; cur != 0 && steps <= entities.size(); ++steps) {
    if (cur == child) return true;
    const Entity3D* p = find(entities, cur);
    cur = p ? p->parent : 0;
  }
  return false;
}

void move_with_children(std::vector<Entity3D>& entities, std::uint64_t id, const Transform& now) {
  Entity3D* e = find(entities, id);
  if (!e) return;
  const Transform prev = e->transform;
  e->transform = now;
  for (std::uint64_t d : descendants_of(entities, id)) {
    if (Entity3D* c = find(entities, d)) carry_child(c, prev, now);
  }
}

bool attach(std::vector<Entity3D>& entities, std::uint64_t child, std::uint64_t target,
            AttachMode mode, std::string* why) {
  Entity3D* c = find(entities, child);
  const Entity3D* t = find(entities, target);
  if (!c || !t) {
    if (why) *why = "no such object";
    return false;
  }
  if (would_loop(entities, child, target)) {
    if (why) *why = c->id == target ? "cannot attach an object to itself"
                                    : "target is one of its own children";
    return false;
  }
  if (mode == AttachMode::SnapToTarget) {
    Transform now = c->transform;
    now.position = t->transform.position;
    now.rotation = t->transform.rotation;
    move_with_children(entities, child, now);
  }
  c = find(entities, child);
  c->parent = target;
  c->parent_bone.clear();
  c->bone_offset = Transform{};
  return true;
}

bool detach(std::vector<Entity3D>& entities, std::uint64_t id) {
  Entity3D* e = find(entities, id);
  if (!e) return false;
  e->parent = 0;
  e->parent_bone.clear();
  e->bone_offset = Transform{};
  return true;
}

bool bone_world(const Entity3D& owner, const render3d::Model& model, const std::string& bone,
                const std::vector<render3d::JointPose>* pose, Mat4* out) {
  const render3d::Skeleton& sk = model.skeleton;
  const int j = sk.find(bone);
  if (j < 0) return false;
  std::vector<Mat4> mats;
  if (pose && pose->size() == sk.joints.size()) {
    render3d::joint_matrices(sk, *pose, &mats);
  } else {
    render3d::joint_matrices(sk, render3d::rest_pose(sk), &mats);
  }
  if (j >= static_cast<int>(mats.size())) return false;
  *out = owner.transform.matrix() * mats[static_cast<std::size_t>(j)];
  return true;
}

bool attach_to_bone(std::vector<Entity3D>& entities, std::uint64_t child, std::uint64_t target,
                    const std::string& bone, const Mat4& bone_world_m, AttachMode mode,
                    std::string* why) {
  if (bone.empty()) {
    if (why) *why = "no bone picked";
    return false;
  }
  if (!attach(entities, child, target, AttachMode::KeepWorld, why)) return false;
  Entity3D* c = find(entities, child);
  if (mode == AttachMode::SnapToTarget) {
    Transform now = decompose(bone_world_m);
    now.scale = c->transform.scale;
    move_with_children(entities, child, now);
    c = find(entities, child);
  }
  c->parent_bone = bone;
  c->bone_offset = decompose(bone_world_m.inverse() * c->transform.matrix());
  return true;
}

Transform bone_follow(const Entity3D& e, const Mat4& bone_world_m) {
  return decompose(bone_world_m * e.bone_offset.matrix());
}

Transform follow_result(const Entity3D& e, const Transform& target) {
  if (!e.follow) return e.transform;
  const Follow3D& f = *e.follow;
  Transform out = e.transform;
  if (f.position) {
    out.position = target.position + (f.rotation ? target.rotation.rotate(f.offset) : f.offset);
  }
  if (f.rotation) out.rotation = render3d::normalize(target.rotation * f.rotation_offset);
  return out;
}

Follow3D follow_from_current(const Transform& e, const Transform& target, bool position,
                             bool rotation, std::uint64_t target_id) {
  Follow3D f;
  f.target = target_id;
  f.position = position;
  f.rotation = rotation;
  const Vec3 d = e.position - target.position;
  f.offset = rotation ? target.rotation.conjugate().rotate(d) : d;
  f.rotation_offset = render3d::normalize(target.rotation.conjugate() * e.rotation);
  return f;
}

Transform copy_transform(const Transform& from, const Transform& to, bool position, bool rotation,
                         bool scale) {
  Transform out = to;
  if (position) out.position = from.position;
  if (rotation) out.rotation = from.rotation;
  if (scale) out.scale = from.scale;
  return out;
}

Transform align_to_surface(const Transform& t, const Vec3& hit, const Vec3& normal) {
  Transform out = t;
  out.position = hit;
  if (length(normal) > 1e-6f) {
    const Vec3 up = t.rotation.rotate({0.0f, 1.0f, 0.0f});
    out.rotation = render3d::normalize(rotation_between(up, normal) * t.rotation);
  }
  return out;
}

}  // namespace scene3d
}  // namespace tombstone
}  // namespace ts
