#pragma once

// Humanoid retarget: play a clip authored for one rig (a Mixamo
// running.fbx, a glTF, a .blend export) on a different skinned character.
// Bones are matched by humanoid slot (hips, spine, arms, legs) after
// stripping Mixamo / Biped prefixes, then by the same bone name. Rotations
// are the change from each rig's rest pose, so a T-pose and an A-pose
// don't fight. Hip travel scales with leg length. Pure CPU.

#include "render3d/Skeleton.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

// Bones drawn in the viewport. Larger rigs still pose and skin; the overlay
// stops here so a crowd of joints cannot stall the editor.
inline constexpr int kMaxDrawnBones = 768;
// Keys kept per channel when a source clip is denser than this.
inline constexpr int kMaxRetargetKeys = 4096;

enum class HumanSlot : std::uint8_t {
  Hips, Spine, Chest, UpperChest, Neck, Head,
  ShoulderL, UpperArmL, LowerArmL, HandL,
  ShoulderR, UpperArmR, LowerArmR, HandR,
  UpperLegL, LowerLegL, FootL, ToesL,
  UpperLegR, LowerLegR, FootR, ToesR,
  Count
};

struct HumanoidMap {
  int joint[static_cast<int>(HumanSlot::Count)];
  HumanoidMap();
  int get(HumanSlot slot) const { return joint[static_cast<int>(slot)]; }
};

// First joint that owns each slot, or -1. "mixamorig:LeftArm", "LeftArm",
// "upper_arm.L" and "upper_arm_l" all land on UpperArmL.
HumanoidMap map_humanoid(const Skeleton& skel);

struct RetargetReport {
  int clips = 0;
  int bones = 0;    // target joints that received a channel
  int dropped = 0;  // source joints with animation and no match
  int keys_capped = 0;
  std::string detail;
};

Clip retarget_clip(const Skeleton& source, const Clip& clip, const Skeleton& target,
                   RetargetReport* report = nullptr);
std::vector<Clip> retarget_clips(const Skeleton& source, const std::vector<Clip>& clips,
                                 const Skeleton& target, RetargetReport* report = nullptr);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
