#include "render3d/Retarget.h"

#include "render3d/ClipFile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace ts {
namespace tombstone {
namespace render3d {

namespace {

struct Alias {
  const char* name;
  HumanSlot slot;
};

// Longer names are listed beside shorter ones; matching is exact on the
// normalized form, so "leftforearm" does not steal "leftarm".
const Alias kAliases[] = {
    {"hips", HumanSlot::Hips},
    {"pelvis", HumanSlot::Hips},
    {"hip", HumanSlot::Hips},
    {"spine", HumanSlot::Spine},
    {"spine1", HumanSlot::Spine},
    {"spine01", HumanSlot::Spine},
    {"spine001", HumanSlot::Spine},
    {"spine2", HumanSlot::Chest},
    {"spine02", HumanSlot::Chest},
    {"spine002", HumanSlot::Chest},
    {"chest", HumanSlot::Chest},
    {"spine3", HumanSlot::UpperChest},
    {"spine03", HumanSlot::UpperChest},
    {"spine003", HumanSlot::UpperChest},
    {"upperchest", HumanSlot::UpperChest},
    {"neck", HumanSlot::Neck},
    {"neck01", HumanSlot::Neck},
    {"neck001", HumanSlot::Neck},
    {"head", HumanSlot::Head},
    {"leftshoulder", HumanSlot::ShoulderL},
    {"leftclavicle", HumanSlot::ShoulderL},
    {"claviclel", HumanSlot::ShoulderL},
    {"shoulderl", HumanSlot::ShoulderL},
    {"lshoulder", HumanSlot::ShoulderL},
    {"rightshoulder", HumanSlot::ShoulderR},
    {"rightclavicle", HumanSlot::ShoulderR},
    {"clavicler", HumanSlot::ShoulderR},
    {"shoulderr", HumanSlot::ShoulderR},
    {"rshoulder", HumanSlot::ShoulderR},
    {"leftarm", HumanSlot::UpperArmL},
    {"leftupperarm", HumanSlot::UpperArmL},
    {"upperarml", HumanSlot::UpperArmL},
    {"upperarmleft", HumanSlot::UpperArmL},
    {"lupperarm", HumanSlot::UpperArmL},
    {"arml", HumanSlot::UpperArmL},
    {"rightarm", HumanSlot::UpperArmR},
    {"rightupperarm", HumanSlot::UpperArmR},
    {"upperarmr", HumanSlot::UpperArmR},
    {"upperarmright", HumanSlot::UpperArmR},
    {"rupperarm", HumanSlot::UpperArmR},
    {"armr", HumanSlot::UpperArmR},
    {"leftforearm", HumanSlot::LowerArmL},
    {"leftlowerarm", HumanSlot::LowerArmL},
    {"lowerarml", HumanSlot::LowerArmL},
    {"forearml", HumanSlot::LowerArmL},
    {"lforearm", HumanSlot::LowerArmL},
    {"rightforearm", HumanSlot::LowerArmR},
    {"rightlowerarm", HumanSlot::LowerArmR},
    {"lowerarmr", HumanSlot::LowerArmR},
    {"forearmr", HumanSlot::LowerArmR},
    {"rforearm", HumanSlot::LowerArmR},
    {"lefthand", HumanSlot::HandL},
    {"handl", HumanSlot::HandL},
    {"handleft", HumanSlot::HandL},
    {"lwrist", HumanSlot::HandL},
    {"righthand", HumanSlot::HandR},
    {"handr", HumanSlot::HandR},
    {"handright", HumanSlot::HandR},
    {"rwrist", HumanSlot::HandR},
    {"leftupleg", HumanSlot::UpperLegL},
    {"leftupperleg", HumanSlot::UpperLegL},
    {"upperlegl", HumanSlot::UpperLegL},
    {"thighl", HumanSlot::UpperLegL},
    {"leftthigh", HumanSlot::UpperLegL},
    {"lupleg", HumanSlot::UpperLegL},
    {"rightupleg", HumanSlot::UpperLegR},
    {"rightupperleg", HumanSlot::UpperLegR},
    {"upperlegr", HumanSlot::UpperLegR},
    {"thighr", HumanSlot::UpperLegR},
    {"rightthigh", HumanSlot::UpperLegR},
    {"rupleg", HumanSlot::UpperLegR},
    {"shinl", HumanSlot::LowerLegL},
    {"leftshin", HumanSlot::LowerLegL},
    {"leftleg", HumanSlot::LowerLegL},
    {"leftlowerleg", HumanSlot::LowerLegL},
    {"lowerlegl", HumanSlot::LowerLegL},
    {"calfl", HumanSlot::LowerLegL},
    {"leftcalf", HumanSlot::LowerLegL},
    {"leftshin", HumanSlot::LowerLegL},
    {"shinr", HumanSlot::LowerLegR},
    {"rightshin", HumanSlot::LowerLegR},
    {"rightleg", HumanSlot::LowerLegR},
    {"rightlowerleg", HumanSlot::LowerLegR},
    {"lowerlegr", HumanSlot::LowerLegR},
    {"calfr", HumanSlot::LowerLegR},
    {"rightcalf", HumanSlot::LowerLegR},
    {"rightshin", HumanSlot::LowerLegR},
    {"leftfoot", HumanSlot::FootL},
    {"footl", HumanSlot::FootL},
    {"footleft", HumanSlot::FootL},
    {"rightfoot", HumanSlot::FootR},
    {"footr", HumanSlot::FootR},
    {"footright", HumanSlot::FootR},
    {"balll", HumanSlot::ToesL},
    {"toel", HumanSlot::ToesL},
    {"lefttoebase", HumanSlot::ToesL},
    {"lefttoe", HumanSlot::ToesL},
    {"toesl", HumanSlot::ToesL},
    {"ballr", HumanSlot::ToesR},
    {"toer", HumanSlot::ToesR},
    {"righttoebase", HumanSlot::ToesR},
    {"righttoe", HumanSlot::ToesR},
    {"toesr", HumanSlot::ToesR},
};

std::string normalize_bone(std::string_view in) {
  const auto bar = in.find_last_of("|:");
  if (bar != std::string_view::npos) in = in.substr(bar + 1);
  std::string out;
  out.reserve(in.size());
  for (unsigned char c : in) {
    if (c == ' ' || c == '_' || c == '.' || c == '-' || c == ':') continue;
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  const std::string prefix = "mixamorig";
  if (out.rfind(prefix, 0) == 0) out.erase(0, prefix.size());
  if (out.rfind("bip01", 0) == 0) out.erase(0, 5);
  if (out.rfind("bip001", 0) == 0) out.erase(0, 6);
  // Blender Rigify: DEF-spine, ORG-thigh.L (hyphens already stripped).
  if (out.rfind("def", 0) == 0 && out.size() > 3) out.erase(0, 3);
  if (out.rfind("org", 0) == 0 && out.size() > 3) out.erase(0, 3);
  return out;
}

const Alias* find_alias(const std::string& name) {
  for (const Alias& a : kAliases) {
    if (name == a.name) return &a;
  }
  return nullptr;
}

float leg_length(const Skeleton& skel, const HumanoidMap& map) {
  const std::vector<Vec3> at = joint_positions(skel, rest_pose(skel));
  auto dist = [&](int a, int b) {
    if (a < 0 || b < 0) return 0.0f;
    return length(at[static_cast<std::size_t>(b)] - at[static_cast<std::size_t>(a)]);
  };
  const float left = dist(map.get(HumanSlot::UpperLegL), map.get(HumanSlot::FootL));
  const float right = dist(map.get(HumanSlot::UpperLegR), map.get(HumanSlot::FootR));
  const float leg = std::max(left, right);
  if (leg > 1e-3f) return leg;
  const int hip = map.get(HumanSlot::Hips);
  if (hip >= 0) return std::max(std::fabs(at[static_cast<std::size_t>(hip)].y), 1e-3f);
  return 1.0f;
}

struct Bind {
  int source = -1;
  int target = -1;
  bool hips = false;
};

std::vector<Bind> binds_for(const Skeleton& source, const Skeleton& target, const HumanoidMap& sm,
                            const HumanoidMap& dm) {
  std::vector<Bind> out;
  std::unordered_set<int> src_used, dst_used;
  for (int s = 0; s < static_cast<int>(HumanSlot::Count); ++s) {
    const int sj = sm.joint[s], dj = dm.joint[s];
    if (sj < 0 || dj < 0) continue;
    out.push_back({sj, dj, static_cast<HumanSlot>(s) == HumanSlot::Hips});
    src_used.insert(sj);
    dst_used.insert(dj);
  }
  std::unordered_map<std::string, int> by_name;
  for (std::size_t i = 0; i < target.joints.size(); ++i) {
    if (dst_used.count(static_cast<int>(i))) continue;
    const std::string n = normalize_bone(target.joints[i].name);
    if (!n.empty() && !by_name.count(n)) by_name.emplace(n, static_cast<int>(i));
  }
  for (std::size_t i = 0; i < source.joints.size(); ++i) {
    if (src_used.count(static_cast<int>(i))) continue;
    const auto it = by_name.find(normalize_bone(source.joints[i].name));
    if (it == by_name.end() || dst_used.count(it->second)) continue;
    out.push_back({static_cast<int>(i), it->second, source.joints[i].parent < 0});
    dst_used.insert(it->second);
  }
  return out;
}

const Channel* find_channel(const Clip& clip, int joint, ChannelPath path) {
  for (const Channel& ch : clip.channels) {
    if (ch.joint == joint && ch.path == path) return &ch;
  }
  return nullptr;
}

void copy_rotation(const Skeleton& source, const Skeleton& target, const Channel& ch, int src_j, int dst_j,
                   Clip* out, int* capped) {
  const Quat rest_s = normalize(source.joints[static_cast<std::size_t>(src_j)].r);
  const Quat rest_d = normalize(target.joints[static_cast<std::size_t>(dst_j)].r);
  const bool dense = static_cast<int>(ch.times.size()) > kMaxRetargetKeys;
  if (dense) ++*capped;
  const std::size_t n = dense ? static_cast<std::size_t>(kMaxRetargetKeys) : ch.times.size();
  if (n == 0) return;
  Channel built;
  built.joint = dst_j;
  built.path = ChannelPath::Rotation;
  built.interp = (!dense && ch.interp == Interp::Step) ? Interp::Step : Interp::Linear;
  const float t0 = ch.times.front(), t1 = ch.times.back();
  for (std::size_t i = 0; i < n; ++i) {
    const float time = dense ? t0 + (t1 - t0) * (n == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(n - 1))
                             : ch.times[i];
    float v[4];
    sample_channel(ch, time, v);
    const Quat q = normalize(Quat{v[0], v[1], v[2], v[3]});
    const Quat delta = normalize(q * rest_s.conjugate());
    const Quat posed = normalize(delta * rest_d);
    const float key[4] = {posed.x, posed.y, posed.z, posed.w};
    upsert_key(&built, time, key);
  }
  if (!built.times.empty()) out->channels.push_back(std::move(built));
}

void copy_hips_translation(const Skeleton& source, const Skeleton& target, const Channel& ch, int src_j,
                           int dst_j, float scale, Clip* out, int* capped) {
  const Vec3 rest_s = source.joints[static_cast<std::size_t>(src_j)].t;
  const Vec3 rest_d = target.joints[static_cast<std::size_t>(dst_j)].t;
  const bool dense = static_cast<int>(ch.times.size()) > kMaxRetargetKeys;
  if (dense) ++*capped;
  const std::size_t n = dense ? static_cast<std::size_t>(kMaxRetargetKeys) : ch.times.size();
  Channel built;
  built.joint = dst_j;
  built.path = ChannelPath::Translation;
  built.interp = (!dense && ch.interp == Interp::Step) ? Interp::Step : Interp::Linear;
  const float t0 = ch.times.front(), t1 = ch.times.back();
  for (std::size_t i = 0; i < n; ++i) {
    const float time = dense ? t0 + (t1 - t0) * (n == 1 ? 0.0f : static_cast<float>(i) / static_cast<float>(n - 1))
                             : ch.times[i];
    float v[4];
    sample_channel(ch, time, v);
    const Vec3 delta = Vec3{v[0], v[1], v[2]} - rest_s;
    const Vec3 posed = rest_d + delta * scale;
    const float key[3] = {posed.x, posed.y, posed.z};
    upsert_key(&built, time, key);
  }
  if (!built.times.empty()) out->channels.push_back(std::move(built));
}

}  // namespace

HumanoidMap::HumanoidMap() {
  for (int& j : joint) j = -1;
}

const char* human_slot_name(HumanSlot slot) {
  static const char* kNames[] = {
      "Hips", "Spine", "Chest", "UpperChest", "Neck", "Head",
      "ShoulderL", "UpperArmL", "LowerArmL", "HandL",
      "ShoulderR", "UpperArmR", "LowerArmR", "HandR",
      "UpperLegL", "LowerLegL", "FootL", "ToesL",
      "UpperLegR", "LowerLegR", "FootR", "ToesR"};
  const int i = static_cast<int>(slot);
  if (i < 0 || i >= static_cast<int>(HumanSlot::Count)) return nullptr;
  return kNames[i];
}

int human_slot_from_name(std::string_view name) {
  for (int i = 0; i < static_cast<int>(HumanSlot::Count); ++i) {
    const char* n = human_slot_name(static_cast<HumanSlot>(i));
    if (n && name.size() == std::char_traits<char>::length(n)) {
      bool eq = true;
      for (std::size_t c = 0; c < name.size(); ++c) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(name[c])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(n[c])));
        if (a != b) { eq = false; break; }
      }
      if (eq) return i;
    }
  }
  return -1;
}

HumanoidMap map_humanoid(const Skeleton& skel) {
  HumanoidMap map;
  for (std::size_t i = 0; i < skel.joints.size(); ++i) {
    const Alias* a = find_alias(normalize_bone(skel.joints[i].name));
    if (!a) continue;
    int& slot = map.joint[static_cast<int>(a->slot)];
    if (slot < 0) slot = static_cast<int>(i);
  }
  return map;
}

Clip retarget_clip(const Skeleton& source, const Clip& clip, const Skeleton& target, RetargetReport* report,
                   const std::vector<AvatarOverride>* avatar) {
  Clip out;
  out.name = clip.name.empty() ? "clip" : clip.name;
  out.events = clip.events;
  const HumanoidMap sm = map_humanoid(source);
  HumanoidMap dm = map_humanoid(target);
  if (avatar) {
    for (const AvatarOverride& o : *avatar) {
      const int slot = human_slot_from_name(o.slot);
      const int bone = target.find(o.bone);
      if (slot < 0 || bone < 0) continue;
      dm.joint[slot] = bone;
    }
  }
  const float src_leg = leg_length(source, sm);
  const float dst_leg = leg_length(target, dm);
  const float scale = std::clamp(dst_leg / std::max(src_leg, 1e-4f), 0.05f, 20.0f);
  const std::vector<Bind> binds = binds_for(source, target, sm, dm);
  std::unordered_set<int> bound_src;
  for (const Bind& b : binds) bound_src.insert(b.source);
  int dropped = 0;
  for (const Channel& ch : clip.channels) {
    if (!bound_src.count(ch.joint)) ++dropped;
  }
  int capped = 0;
  std::unordered_set<int> written;
  for (const Bind& b : binds) {
    bool any = false;
    if (const Channel* rot = find_channel(clip, b.source, ChannelPath::Rotation)) {
      copy_rotation(source, target, *rot, b.source, b.target, &out, &capped);
      any = true;
    }
    if (b.hips) {
      if (const Channel* tr = find_channel(clip, b.source, ChannelPath::Translation)) {
        copy_hips_translation(source, target, *tr, b.source, b.target, scale, &out, &capped);
        any = true;
      }
    }
    if (any) written.insert(b.target);
  }
  clip_duration(&out);
  if (out.duration <= 0.0f) out.duration = clip.duration;
  if (report) {
    report->bones += static_cast<int>(written.size());
    report->dropped += dropped;
    report->keys_capped += capped;
  }
  return out;
}

std::vector<Clip> retarget_clips(const Skeleton& source, const std::vector<Clip>& clips, const Skeleton& target,
                                 RetargetReport* report, const std::vector<AvatarOverride>* avatar) {
  std::vector<Clip> out;
  out.reserve(clips.size());
  for (const Clip& c : clips) {
    if (c.channels.empty() && c.events.empty()) continue;
    Clip mapped = retarget_clip(source, c, target, report, avatar);
    if (mapped.channels.empty() && mapped.events.empty()) continue;
    out.push_back(std::move(mapped));
    if (report) ++report->clips;
  }
  if (report && report->detail.empty()) {
    report->detail = std::to_string(report->clips) + " clip(s), " + std::to_string(report->bones) +
                     " bone(s) matched, " + std::to_string(report->dropped) + " source channel(s) had no bone";
    if (report->keys_capped) report->detail += "; dense curves were resampled";
  }
  return out;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
