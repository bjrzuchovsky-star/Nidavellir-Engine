#pragma once

// Authored skeletal clips (.anim3d.json) beside a rigged model, and the
// bone-line data the viewport overlay draws. Import keeps the file's own
// clips; a sibling "<stem>.anim3d.json" replaces a clip of the same name
// and appends new ones. Joints are stored by name so a reimport that keeps
// bone names still binds. The timeline editor writes that file. Pure data:
// the same clips the GPU skins.

#include "render3d/Skeleton.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

struct Model;

struct BoneSegment {
  int parent = -1;  // -1 = a root (from == to; the overlay draws a marker)
  int joint = 0;
  Vec3 from;
  Vec3 to;
};

// Joint origins in model space. A pose of the wrong length uses the rest
// pose. A palette is the skinning palette (joint * inverse bind); a size
// mismatch falls back to the rest pose.
std::vector<Vec3> joint_positions(const Skeleton& skel, const Pose& pose);
std::vector<Vec3> joint_positions(const Skeleton& skel, const std::vector<Mat4>& palette);
// One segment per joint (roots included, so the overlay can mark them).
std::vector<BoneSegment> bone_segments(const Skeleton& skel, const Pose& pose);

// "assets/hero.gltf" -> "assets/hero.anim3d.json".
std::string clip_sidecar_path(const std::string& model_path);

// Read or write a sidecar. A missing file is not an error for load (true,
// empty). `error` on a successful read lists unknown joint names.
bool load_clip_file(const std::string& path, const Skeleton& skel, std::vector<Clip>* out,
                    std::string* error);
bool save_clip_file(const std::string& path, const Skeleton& skel, const std::vector<Clip>& clips,
                    std::string* error);

// Parse / write the sidecar document. Unknown joints are dropped and named
// in `error`; a document that does not parse returns false.
bool clips_from_json(const std::string& text, const Skeleton& skel, std::vector<Clip>* out,
                     std::string* error);
std::string clips_to_json(const Skeleton& skel, const std::vector<Clip>& clips);

// Merge "<model>.anim3d.json" into model->clips. No sidecar: no change.
// A file that does not parse leaves the imported clips and returns false.
bool merge_clip_sidecar(Model* model, std::string* warning);

// Key editing (the timeline). Times within 1e-4 s are the same key.
Channel* ensure_channel(Clip* clip, int joint, ChannelPath path, Interp interp);
// `value` is width() floats. Cubic keys take optional tangents (null = 0).
// Returns the key index, or -1.
int upsert_key(Channel* ch, float time, const float* value, const float* in_tangent = nullptr,
               const float* out_tangent = nullptr);
bool erase_key(Channel* ch, int index);
// Removes the key and inserts it at `new_time` (a key already there is replaced).
int move_key(Channel* ch, int index, float new_time);
// Rebuild values when the interpolation changes (cubic tangents start at 0).
void set_channel_interp(Channel* ch, Interp interp);
void clip_duration(Clip* clip);
void pose_channel_value(const JointPose& jp, ChannelPath path, float* out);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
