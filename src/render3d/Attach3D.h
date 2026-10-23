#pragma once

// Attach / follow / snap helpers for 3D entities (editor tools, runtime
// and smokes share them). Entities keep world-space transforms; a parent
// carries its children by its own deltas (scene3d::carry_child), so
// "attach keeping position" is only a parent change and "snap to target"
// moves the object onto the target first. See docs/transforms.md.

#include "render3d/Math3D.h"
#include "render3d/Scene3D.h"
#include "render3d/Skeleton.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {
struct Model;
}
namespace scene3d {

using render3d::Mat4;

enum class AttachMode : std::uint8_t { KeepWorld, SnapToTarget };

// Rotation from an orthonormal basis (columns x, y, z).
Quat quat_from_basis(const Vec3& x, const Vec3& y, const Vec3& z);
// Rotation whose -Z looks along `forward` (cameras, lights).
Quat look_rotation(const Vec3& forward, const Vec3& up = {0.0f, 1.0f, 0.0f});
// Shortest rotation taking direction a onto b.
Quat rotation_between(const Vec3& a, const Vec3& b);
// TRS of an affine matrix (no shear).
Transform decompose(const Mat4& m);

Entity3D* find(std::vector<Entity3D>& entities, std::uint64_t id);
const Entity3D* find(const std::vector<Entity3D>& entities, std::uint64_t id);
// Would parenting `child` under `target` close a loop (or is it itself)?
bool would_loop(const std::vector<Entity3D>& entities, std::uint64_t child, std::uint64_t target);

// Move one entity to `now` and carry its descendants along.
void move_with_children(std::vector<Entity3D>& entities, std::uint64_t id, const Transform& now);

// Parent `child` under `target`. Snap puts it on the target's position and
// rotation first (scale kept). Clears any bone link. False (and *why) for
// missing ids, loops or self.
bool attach(std::vector<Entity3D>& entities, std::uint64_t child, std::uint64_t target,
            AttachMode mode, std::string* why = nullptr);
// Back to the scene root, world transform unchanged.
bool detach(std::vector<Entity3D>& entities, std::uint64_t id);

// World matrix of `bone` on the skinned model of `owner` for `pose`
// (empty pose = the rest pose). False when the model has no such bone.
bool bone_world(const Entity3D& owner, const render3d::Model& model, const std::string& bone,
                const std::vector<render3d::JointPose>* pose, Mat4* out);
// Attach to a bone of `target`'s model: keep = bone_offset holds the
// current world placement in bone space; snap = onto the bone.
bool attach_to_bone(std::vector<Entity3D>& entities, std::uint64_t child, std::uint64_t target,
                    const std::string& bone, const Mat4& bone_world_m, AttachMode mode,
                    std::string* why = nullptr);
// Where a bone-attached entity sits for `bone_world_m`.
Transform bone_follow(const Entity3D& e, const Mat4& bone_world_m);

// Follow constraint target transform for `e` (unchanged without one).
Transform follow_result(const Entity3D& e, const Transform& target);
// Offsets that keep `e` exactly where it is relative to `target`.
Follow3D follow_from_current(const Transform& e, const Transform& target, bool position,
                             bool rotation, std::uint64_t target_id);

// Copy chosen channels of `from` onto `to`.
Transform copy_transform(const Transform& from, const Transform& to, bool position,
                         bool rotation, bool scale);
// Sit on a surface point: position = hit, local +Y turned onto the normal
// (the smallest turn from where it faces now).
Transform align_to_surface(const Transform& t, const Vec3& hit, const Vec3& normal);

}  // namespace scene3d
}  // namespace tombstone
}  // namespace ts
