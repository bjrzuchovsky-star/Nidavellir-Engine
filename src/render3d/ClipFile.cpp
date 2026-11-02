#include "render3d/ClipFile.h"

#include "render3d/Gltf.h"
#include "render3d/Json.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace render3d {

namespace fs = std::filesystem;

namespace {

constexpr float kKeyEps = 1e-4f;

std::size_t value_stride(const Channel& ch) {
  return static_cast<std::size_t>(ch.width()) * (ch.interp == Interp::CubicSpline ? 3 : 1);
}

void read_key(const Channel& ch, std::size_t k, float* value, float* in_t, float* out_t) {
  const int w = ch.width();
  const std::size_t st = value_stride(ch);
  const float* base = ch.values.data() + k * st;
  if (ch.interp == Interp::CubicSpline) {
    if (in_t) std::copy(base, base + w, in_t);
    std::copy(base + w, base + 2 * w, value);
    if (out_t) std::copy(base + 2 * w, base + 3 * w, out_t);
  } else {
    std::copy(base, base + w, value);
    if (in_t) std::fill(in_t, in_t + w, 0.0f);
    if (out_t) std::fill(out_t, out_t + w, 0.0f);
  }
}

const char* path_name(ChannelPath path) {
  switch (path) {
    case ChannelPath::Rotation: return "rotation";
    case ChannelPath::Scale: return "scale";
    default: return "translation";
  }
}

bool path_from(const std::string& s, ChannelPath* out) {
  if (s == "rotation" || s == "r") {
    *out = ChannelPath::Rotation;
    return true;
  }
  if (s == "scale" || s == "s") {
    *out = ChannelPath::Scale;
    return true;
  }
  if (s == "translation" || s == "t" || s == "position") {
    *out = ChannelPath::Translation;
    return true;
  }
  return false;
}

const char* interp_name(Interp interp) {
  switch (interp) {
    case Interp::Step: return "step";
    case Interp::CubicSpline: return "cubic";
    default: return "linear";
  }
}

bool interp_from(const std::string& s, Interp* out) {
  if (s == "step") {
    *out = Interp::Step;
    return true;
  }
  if (s == "cubic" || s == "cubicspline" || s == "cubic_spline") {
    *out = Interp::CubicSpline;
    return true;
  }
  if (s == "linear" || s.empty()) {
    *out = Interp::Linear;
    return true;
  }
  return false;
}

bool read_n(const json::Value& arr, float* out, int n) {
  if (!arr.is_array() || static_cast<int>(arr.size()) < n) return false;
  for (int i = 0; i < n; ++i) {
    if (!arr[static_cast<std::size_t>(i)].is_number()) return false;
    out[i] = arr[static_cast<std::size_t>(i)].as_float();
  }
  return true;
}

std::string read_file(const fs::path& path, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "could not read " + path.string();
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

std::vector<Vec3> joint_positions(const Skeleton& skel, const Pose& pose) {
  Pose rest;
  const Pose* use = &pose;
  if (pose.size() != skel.joints.size()) {
    rest = rest_pose(skel);
    use = &rest;
  }
  std::vector<Mat4> jm;
  joint_matrices(skel, *use, &jm);
  std::vector<Vec3> out(jm.size());
  for (std::size_t i = 0; i < jm.size(); ++i) out[i] = {jm[i].at(0, 3), jm[i].at(1, 3), jm[i].at(2, 3)};
  return out;
}

std::vector<Vec3> joint_positions(const Skeleton& skel, const std::vector<Mat4>& palette) {
  if (palette.size() != skel.joints.size()) return joint_positions(skel, Pose{});
  std::vector<Vec3> out(skel.joints.size());
  for (std::size_t i = 0; i < skel.joints.size(); ++i) {
    // Palette is joint * inverse bind, so multiplying the bind back yields
    // the joint matrix the overlay (and a CPU skin) already agree on.
    const Mat4 jm = palette[i] * skel.joints[i].inverse_bind.inverse();
    out[i] = {jm.at(0, 3), jm.at(1, 3), jm.at(2, 3)};
  }
  return out;
}

std::vector<BoneSegment> bone_segments(const Skeleton& skel, const Pose& pose) {
  const std::vector<Vec3> at = joint_positions(skel, pose);
  std::vector<BoneSegment> out;
  out.reserve(skel.joints.size());
  for (std::size_t i = 0; i < skel.joints.size() && i < at.size(); ++i) {
    BoneSegment s;
    s.joint = static_cast<int>(i);
    s.parent = skel.joints[i].parent;
    s.to = at[i];
    s.from = (s.parent >= 0 && static_cast<std::size_t>(s.parent) < at.size()) ? at[static_cast<std::size_t>(s.parent)]
                                                                               : s.to;
    out.push_back(s);
  }
  return out;
}

std::string clip_sidecar_path(const std::string& model_path) {
  fs::path p = fs::u8path(model_path);
  p.replace_extension(".anim3d.json");
  return p.string();
}

bool clips_from_json(const std::string& text, const Skeleton& skel, std::vector<Clip>* out,
                     std::string* error) {
  if (!out) return false;
  out->clear();
  json::Value root;
  std::string err;
  if (!json::parse(text, &root, &err) || !root.is_object()) {
    if (error) *error = err.empty() ? "anim3d: not a JSON object" : err;
    return false;
  }
  const json::Value& clips = root["clips"];
  if (!clips.is_array()) {
    if (error) *error = "anim3d: missing clips array";
    return false;
  }
  std::string unknown;
  auto note = [&](const std::string& name) {
    if (name.empty() || unknown.find("'" + name + "'") != std::string::npos) return;
    unknown += (unknown.empty() ? "" : ", ") + ("'" + name + "'");
  };
  for (std::size_t i = 0; i < clips.size(); ++i) {
    const json::Value& jc = clips[i];
    if (!jc.is_object()) continue;
    Clip clip;
    clip.name = jc.str("name");
    if (clip.name.empty()) clip.name = "clip" + std::to_string(i);
    const json::Value& channels = jc["channels"];
    if (channels.is_array()) {
      for (std::size_t c = 0; c < channels.size(); ++c) {
        const json::Value& jch = channels[c];
        if (!jch.is_object()) continue;
        const std::string joint_name = jch.str("joint");
        const int joint = skel.find(joint_name);
        if (joint < 0) {
          note(joint_name.empty() ? "(unnamed joint)" : joint_name);
          continue;
        }
        ChannelPath path = ChannelPath::Translation;
        if (!path_from(jch.str("path", "translation"), &path)) {
          note(joint_name + " path");
          continue;
        }
        Interp interp = Interp::Linear;
        if (!interp_from(jch.str("interp", "linear"), &interp)) interp = Interp::Linear;
        Channel ch;
        ch.joint = joint;
        ch.path = path;
        ch.interp = interp;
        const json::Value& keys = jch["keys"];
        if (!keys.is_array()) continue;
        for (std::size_t k = 0; k < keys.size(); ++k) {
          const json::Value& key = keys[k];
          if (!key.is_object() || !key["t"].is_number()) continue;
          const float t = key["t"].as_float();
          if (!std::isfinite(t)) continue;
          float value[4] = {0, 0, 0, 1};
          float in_t[4] = {};
          float out_t[4] = {};
          const int w = ch.width();
          if (!read_n(key["v"], value, w)) continue;
          const bool has_in = read_n(key["in"], in_t, w);
          const bool has_out = read_n(key["out"], out_t, w);
          upsert_key(&ch, t, value, has_in ? in_t : nullptr, has_out ? out_t : nullptr);
        }
        if (!ch.times.empty()) clip.channels.push_back(std::move(ch));
      }
    }
    clip_duration(&clip);
    out->push_back(std::move(clip));
  }
  if (error) *error = unknown.empty() ? std::string() : "unknown joints: " + unknown;
  return true;
}

std::string clips_to_json(const Skeleton& skel, const std::vector<Clip>& clips) {
  json::Value root = json::Value::object();
  root.set("version", json::Value::number(1));
  json::Value arr = json::Value::array();
  for (const Clip& clip : clips) {
    json::Value jc = json::Value::object();
    jc.set("name", json::Value::string(clip.name));
    json::Value channels = json::Value::array();
    for (const Channel& ch : clip.channels) {
      if (ch.joint < 0 || static_cast<std::size_t>(ch.joint) >= skel.joints.size()) continue;
      if (ch.times.empty()) continue;
      json::Value jch = json::Value::object();
      jch.set("joint", json::Value::string(skel.joints[static_cast<std::size_t>(ch.joint)].name));
      jch.set("path", json::Value::string(path_name(ch.path)));
      jch.set("interp", json::Value::string(interp_name(ch.interp)));
      json::Value keys = json::Value::array();
      const int w = ch.width();
      for (std::size_t k = 0; k < ch.times.size(); ++k) {
        if (ch.values.size() < (k + 1) * value_stride(ch)) break;
        float value[4], in_t[4], out_t[4];
        read_key(ch, k, value, in_t, out_t);
        json::Value key = json::Value::object();
        key.set("t", json::Value::number(ch.times[k]));
        key.set("v", json::floats(value, static_cast<std::size_t>(w)));
        if (ch.interp == Interp::CubicSpline) {
          key.set("in", json::floats(in_t, static_cast<std::size_t>(w)));
          key.set("out", json::floats(out_t, static_cast<std::size_t>(w)));
        }
        keys.push(std::move(key));
      }
      jch.set("keys", std::move(keys));
      channels.push(std::move(jch));
    }
    jc.set("channels", std::move(channels));
    arr.push(std::move(jc));
  }
  root.set("clips", std::move(arr));
  return json::write(root);
}

bool load_clip_file(const std::string& path, const Skeleton& skel, std::vector<Clip>* out,
                    std::string* error) {
  if (!out) return false;
  out->clear();
  std::error_code ec;
  if (!fs::exists(fs::u8path(path), ec)) return true;
  const std::string text = read_file(fs::u8path(path), error);
  if (text.empty() && error && !error->empty()) return false;
  return clips_from_json(text, skel, out, error);
}

bool save_clip_file(const std::string& path, const Skeleton& skel, const std::vector<Clip>& clips,
                    std::string* error) {
  const fs::path p = fs::u8path(path);
  std::error_code ec;
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary);
  if (!out) {
    if (error) *error = "could not write " + path;
    return false;
  }
  const std::string text = clips_to_json(skel, clips);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!out) {
    if (error) *error = "could not write " + path;
    return false;
  }
  return true;
}

bool merge_clip_sidecar(Model* model, std::string* warning) {
  if (!model) return false;
  if (model->source.empty()) return true;
  const std::string path = clip_sidecar_path(model->source);
  std::error_code ec;
  if (!fs::exists(fs::u8path(path), ec)) return true;
  std::vector<Clip> extra;
  std::string err;
  if (!load_clip_file(path, model->skeleton, &extra, &err)) {
    if (warning) *warning = path + ": " + (err.empty() ? "could not read clips" : err);
    return false;
  }
  for (Clip& c : extra) {
    const int i = model->find_clip(c.name);
    if (i >= 0) model->clips[static_cast<std::size_t>(i)] = std::move(c);
    else model->clips.push_back(std::move(c));
  }
  if (warning && !err.empty()) *warning = err;
  return true;
}

Channel* ensure_channel(Clip* clip, int joint, ChannelPath path, Interp interp) {
  if (!clip || joint < 0) return nullptr;
  for (Channel& ch : clip->channels) {
    if (ch.joint == joint && ch.path == path) return &ch;
  }
  Channel ch;
  ch.joint = joint;
  ch.path = path;
  ch.interp = interp;
  clip->channels.push_back(std::move(ch));
  return &clip->channels.back();
}

int upsert_key(Channel* ch, float time, const float* value, const float* in_tangent,
               const float* out_tangent) {
  if (!ch || !value || !std::isfinite(time)) return -1;
  const int w = ch->width();
  const bool cubic = ch->interp == Interp::CubicSpline;
  const std::size_t st = value_stride(*ch);
  auto write_at = [&](std::size_t k) {
    float* dst = ch->values.data() + k * st;
    if (cubic) {
      if (in_tangent) std::copy(in_tangent, in_tangent + w, dst);
      else std::fill(dst, dst + w, 0.0f);
      std::copy(value, value + w, dst + w);
      if (out_tangent) std::copy(out_tangent, out_tangent + w, dst + 2 * static_cast<std::size_t>(w));
      else std::fill(dst + 2 * w, dst + 3 * w, 0.0f);
    } else {
      std::copy(value, value + w, dst);
    }
  };
  for (std::size_t i = 0; i < ch->times.size(); ++i) {
    if (std::fabs(ch->times[i] - time) <= kKeyEps) {
      if (ch->values.size() < (i + 1) * st) ch->values.resize((i + 1) * st, 0.0f);
      write_at(i);
      ch->times[i] = time;
      return static_cast<int>(i);
    }
  }
  const auto it = std::upper_bound(ch->times.begin(), ch->times.end(), time);
  const std::size_t k = static_cast<std::size_t>(it - ch->times.begin());
  ch->times.insert(ch->times.begin() + static_cast<std::ptrdiff_t>(k), time);
  ch->values.insert(ch->values.begin() + static_cast<std::ptrdiff_t>(k * st), st, 0.0f);
  write_at(k);
  return static_cast<int>(k);
}

bool erase_key(Channel* ch, int index) {
  if (!ch || index < 0 || static_cast<std::size_t>(index) >= ch->times.size()) return false;
  const std::size_t st = value_stride(*ch);
  const std::size_t k = static_cast<std::size_t>(index);
  ch->times.erase(ch->times.begin() + index);
  if (ch->values.size() >= (k + 1) * st) {
    ch->values.erase(ch->values.begin() + static_cast<std::ptrdiff_t>(k * st),
                     ch->values.begin() + static_cast<std::ptrdiff_t>((k + 1) * st));
  }
  return true;
}

int move_key(Channel* ch, int index, float new_time) {
  if (!ch || index < 0 || static_cast<std::size_t>(index) >= ch->times.size() || !std::isfinite(new_time))
    return -1;
  float value[4] = {}, in_t[4] = {}, out_t[4] = {};
  read_key(*ch, static_cast<std::size_t>(index), value, in_t, out_t);
  const bool cubic = ch->interp == Interp::CubicSpline;
  erase_key(ch, index);
  return upsert_key(ch, new_time, value, cubic ? in_t : nullptr, cubic ? out_t : nullptr);
}

void set_channel_interp(Channel* ch, Interp interp) {
  if (!ch || ch->interp == interp) return;
  const int w = ch->width();
  const std::size_t n = ch->times.size();
  std::vector<float> values(n * static_cast<std::size_t>(w));
  std::vector<float> ins(n * static_cast<std::size_t>(w));
  std::vector<float> outs(n * static_cast<std::size_t>(w));
  for (std::size_t k = 0; k < n; ++k) {
    read_key(*ch, k, values.data() + k * static_cast<std::size_t>(w),
             ins.data() + k * static_cast<std::size_t>(w), outs.data() + k * static_cast<std::size_t>(w));
  }
  ch->interp = interp;
  const bool cubic = interp == Interp::CubicSpline;
  const std::size_t st = static_cast<std::size_t>(w) * (cubic ? 3 : 1);
  ch->values.assign(n * st, 0.0f);
  for (std::size_t k = 0; k < n; ++k) {
    float* dst = ch->values.data() + k * st;
    const float* v = values.data() + k * static_cast<std::size_t>(w);
    if (cubic) {
      std::copy(v, v + w, dst + w);
    } else {
      std::copy(v, v + w, dst);
    }
  }
}

void clip_duration(Clip* clip) {
  if (!clip) return;
  float d = 0.0f;
  for (const Channel& ch : clip->channels) {
    for (float t : ch.times) d = std::max(d, t);
  }
  clip->duration = d;
}

void pose_channel_value(const JointPose& jp, ChannelPath path, float* out) {
  if (!out) return;
  if (path == ChannelPath::Rotation) {
    out[0] = jp.r.x;
    out[1] = jp.r.y;
    out[2] = jp.r.z;
    out[3] = jp.r.w;
  } else if (path == ChannelPath::Scale) {
    out[0] = jp.s.x;
    out[1] = jp.s.y;
    out[2] = jp.s.z;
  } else {
    out[0] = jp.t.x;
    out[1] = jp.t.y;
    out[2] = jp.t.z;
  }
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
