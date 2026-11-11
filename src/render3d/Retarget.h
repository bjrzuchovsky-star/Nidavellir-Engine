#pragma once

// Humanoid retarget: play a clip authored for one rig (a Mixamo
// running.fbx, a glTF, a .blend export) on a different skinned character.
// Bones are matched by humanoid slot (hips, spine, arms, legs) after
// stripping Mixamo / Biped prefixes, then by the same bone name. Rotations
// are the change from each rig's rest pose, so a T-pose and an A-pose
// don't fight. Hip travel scales with leg length. Pure CPU.

#include "render3d/Skeleton.h"

#include <string>
#include <string_view>
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
// "upper_arm.L", "upperarm_l" (Unreal) and "DEF-thigh.L" (Rigify) all land
// on the same slot.
HumanoidMap map_humanoid(const Skeleton& skel);

// "Hips", "UpperArmL", ... in HumanSlot order. Null when out of range.
const char* human_slot_name(HumanSlot slot);
// -1 when `name` is not a slot. Case-insensitive.
int human_slot_from_name(std::string_view name);
// Comma-separated humanoid slots the auto-map did not find ("Head, ToesL").
// Empty when every slot has a bone. Face and twist bones are not slots.
std::string missing_humanoid_slots(const Skeleton& skel);

// A hand-fixed slot on the destination rig (the avatar / IK map).
// `slot` is human_slot_name ("Hips"); `bone` is that skeleton's joint.
struct AvatarOverride {
  std::string slot;
  std::string bone;
};

struct RetargetReport {
  int clips = 0;
  int bones = 0;    // target joints that received a channel
  int dropped = 0;  // source joints with animation and no match
  int keys_capped = 0;
  std::string detail;
};

Clip retarget_clip(const Skeleton& source, const Clip& clip, const Skeleton& target,
                   RetargetReport* report = nullptr,
                   const std::vector<AvatarOverride>* avatar = nullptr);
std::vector<Clip> retarget_clips(const Skeleton& source, const std::vector<Clip>& clips,
                                 const Skeleton& target, RetargetReport* report = nullptr,
                                 const std::vector<AvatarOverride>* avatar = nullptr);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
