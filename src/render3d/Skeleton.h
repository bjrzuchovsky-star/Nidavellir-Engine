#pragma once

// Skeletal animation core (CPU, no GL): a skeleton of joints with inverse
// bind matrices (glTF skins), animation clips of TRS channels (linear,
// step, cubic spline), pose sampling and blending (per-joint weights for
// masked layers), model-space joint matrices and the skinning palette the
// GPU (GlRenderer3D) or the CPU reference (skin_point) applies to vertices
// carrying up to four joint weights.

#include "render3d/Math3D.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

// Bones per skinned draw (GlRenderer3D bone palette UBO: 128 mat4 = 8 KB,
// inside GL 3.3's guaranteed 16 KB uniform block).
inline constexpr int kMaxBones = 128;

struct Joint {
  std::string name;
  int parent = -1;     // index into Skeleton::joints, -1 = root
  Mat4 inverse_bind;   // mesh space -> joint space at bind time
  Mat4 base;           // model-space transform of a root joint's non-joint parents
  Vec3 t;              // rest pose (the node's own TRS)
  Quat r;
  Vec3 s{1.0f, 1.0f, 1.0f};
};

struct Skeleton {
  std::vector<Joint> joints;  // parents always come before children
  bool empty() const { return joints.empty(); }
  int find(const std::string& name) const;
  // Joints under (and including) `root`; empty name = all.
  std::vector<float> mask_for(const std::vector<std::string>& roots) const;
};

struct JointPose {
  Vec3 t;
  Quat r;
  Vec3 s{1.0f, 1.0f, 1.0f};
};
using Pose = std::vector<JointPose>;

enum class Interp : std::uint8_t { Linear, Step, CubicSpline };
enum class ChannelPath : std::uint8_t { Translation, Rotation, Scale };

struct Channel {
  int joint = 0;
  ChannelPath path = ChannelPath::Translation;
  Interp interp = Interp::Linear;
  std::vector<float> times;   // seconds, ascending
  // Keyframe values, 3 (T / S) or 4 (R, xyzw) floats per key; cubic spline
  // stores in-tangent, value, out-tangent per key (glTF layout).
  std::vector<float> values;
  int width() const { return path == ChannelPath::Rotation ? 4 : 3; }
};

struct Clip {
  std::string name;
  float duration = 0.0f;  // last key time
  std::vector<Channel> channels;
};

Pose rest_pose(const Skeleton& skel);
// Overwrite the joints the clip animates with its values at `time`
// (looped into 0..duration when `loop`, else clamped).
void sample_clip(const Clip& clip, float time, bool loop, Pose* pose);
// One channel's value at `time` (3 or 4 floats into out).
void sample_channel(const Channel& ch, float time, float* out);
// out = a..b by w (joint-wise; mask, when given, scales w per joint).
// Rotations take the short way (nlerp after sign fix, then normalized).
void blend_poses(const Pose& a, const Pose& b, float w, Pose* out,
                 const std::vector<float>* mask = nullptr);
// Model-space joint matrices for a pose.
void joint_matrices(const Skeleton& skel, const Pose& pose, std::vector<Mat4>* out);
// Skinning palette: joint matrix * inverse bind, one per joint (capped at
// kMaxBones).
void skin_palette(const Skeleton& skel, const Pose& pose, std::vector<Mat4>* out);
// CPU reference skinning of one point / direction (weights normalised).
Vec3 skin_point(const std::vector<Mat4>& palette, const float joints[4],
                const float weights[4], const Vec3& p);
Vec3 skin_dir(const std::vector<Mat4>& palette, const float joints[4],
              const float weights[4], const Vec3& d);

Quat slerp(const Quat& a, const Quat& b, float t);

// 1D blend space: weights for clips placed at `points` (ascending or not)
// for `value`. Two neighbours share the weight linearly; outside the range
// the nearest end gets it all. Sum is 1 (empty -> empty).
std::vector<float> blend_weights_1d(const std::vector<float>& points, float value);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
