#include "render3d/Skeleton.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace render3d {

int Skeleton::find(const std::string& name) const {
  for (std::size_t i = 0; i < joints.size(); ++i) {
    if (joints[i].name == name) return static_cast<int>(i);
  }
  return -1;
}

std::vector<float> Skeleton::mask_for(const std::vector<std::string>& roots) const {
  std::vector<float> m(joints.size(), 0.0f);
  bool any = false;
  for (const std::string& r : roots) {
    if (!r.empty()) any = true;
  }
  if (!any) {
    std::fill(m.begin(), m.end(), 1.0f);
    return m;
  }
  for (std::size_t i = 0; i < joints.size(); ++i) {
    for (const std::string& r : roots) {
      if (!r.empty() && joints[i].name == r) m[i] = 1.0f;
    }
    // Parents come first, so one pass carries the mask down the tree.
    const int p = joints[i].parent;
    if (p >= 0 && static_cast<std::size_t>(p) < i && m[static_cast<std::size_t>(p)] > 0.0f) m[i] = 1.0f;
  }
  return m;
}

Pose rest_pose(const Skeleton& skel) {
  Pose p(skel.joints.size());
  for (std::size_t i = 0; i < skel.joints.size(); ++i) {
    p[i].t = skel.joints[i].t;
    p[i].r = skel.joints[i].r;
    p[i].s = skel.joints[i].s;
  }
  return p;
}

Quat slerp(const Quat& a, const Quat& b0, float t) {
  Quat b = b0;
  float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  if (d < 0.0f) {
    b = {-b.x, -b.y, -b.z, -b.w};
    d = -d;
  }
  if (d > 0.9995f) {
    return normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                          a.w + (b.w - a.w) * t});
  }
  const float th = std::acos(std::clamp(d, -1.0f, 1.0f));
  const float s = std::sin(th);
  const float wa = std::sin((1.0f - t) * th) / s, wb = std::sin(t * th) / s;
  return normalize(Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb,
                        a.w * wa + b.w * wb});
}

void sample_channel(const Channel& ch, float time, float* out) {
  const int w = ch.width();
  const std::size_t n = ch.times.size();
  const bool cubic = ch.interp == Interp::CubicSpline;
  const std::size_t stride = static_cast<std::size_t>(w) * (cubic ? 3 : 1);
  auto value = [&](std::size_t k) { return &ch.values[k * stride + (cubic ? w : 0)]; };
  if (n == 0 || ch.values.size() < n * stride) {
    for (int i = 0; i < w; ++i) out[i] = (w == 4 && i == 3) ? 1.0f : 0.0f;
    return;
  }
  if (n == 1 || time <= ch.times.front()) {
    std::copy(value(0), value(0) + w, out);
    return;
  }
  if (time >= ch.times.back()) {
    std::copy(value(n - 1), value(n - 1) + w, out);
    return;
  }
  const auto it = std::upper_bound(ch.times.begin(), ch.times.end(), time);
  const std::size_t k1 = static_cast<std::size_t>(it - ch.times.begin());
  const std::size_t k0 = k1 - 1;
  const float t0 = ch.times[k0], t1 = ch.times[k1];
  const float td = std::max(t1 - t0, 1e-6f);
  const float s = std::clamp((time - t0) / td, 0.0f, 1.0f);
  if (ch.interp == Interp::Step) {
    std::copy(value(k0), value(k0) + w, out);
    return;
  }
  if (cubic) {
    const float* v0 = value(k0);
    const float* b0 = &ch.values[k0 * stride + 2 * w];  // out-tangent of k0
    const float* a1 = &ch.values[k1 * stride];          // in-tangent of k1
    const float* v1 = value(k1);
    const float s2 = s * s, s3 = s2 * s;
    const float h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s;
    const float h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
    for (int i = 0; i < w; ++i) out[i] = h00 * v0[i] + h10 * td * b0[i] + h01 * v1[i] + h11 * td * a1[i];
    if (w == 4) {
      const Quat q = normalize(Quat{out[0], out[1], out[2], out[3]});
      out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
    }
    return;
  }
  const float* a = value(k0);
  const float* b = value(k1);
  if (w == 4) {
    const Quat q = slerp({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, s);
    out[0] = q.x; out[1] = q.y; out[2] = q.z; out[3] = q.w;
    return;
  }
  for (int i = 0; i < w; ++i) out[i] = a[i] + (b[i] - a[i]) * s;
}

void sample_clip(const Clip& clip, float time, bool loop, Pose* pose) {
  if (!pose) return;
  float t = time;
  if (clip.duration > 0.0f) {
    if (loop) {
      t = std::fmod(t, clip.duration);
      if (t < 0.0f) t += clip.duration;
    } else {
      t = std::clamp(t, 0.0f, clip.duration);
    }
  } else {
    t = 0.0f;
  }
  for (const Channel& ch : clip.channels) {
    if (ch.joint < 0 || static_cast<std::size_t>(ch.joint) >= pose->size()) continue;
    float v[4];
    sample_channel(ch, t, v);
    JointPose& jp = (*pose)[static_cast<std::size_t>(ch.joint)];
    switch (ch.path) {
      case ChannelPath::Translation: jp.t = {v[0], v[1], v[2]}; break;
      case ChannelPath::Rotation: jp.r = normalize(Quat{v[0], v[1], v[2], v[3]}); break;
      case ChannelPath::Scale: jp.s = {v[0], v[1], v[2]}; break;
    }
  }
}

void blend_poses(const Pose& a, const Pose& b, float w, Pose* out, const std::vector<float>* mask) {
  const std::size_t n = std::min(a.size(), b.size());
  if (out->size() != n) out->resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    float k = w;
    if (mask) k *= i < mask->size() ? (*mask)[i] : 0.0f;
    k = std::clamp(k, 0.0f, 1.0f);
    JointPose r;
    r.t = lerp(a[i].t, b[i].t, k);
    r.s = lerp(a[i].s, b[i].s, k);
    r.r = slerp(a[i].r, b[i].r, k);
    (*out)[i] = r;
  }
}

void joint_matrices(const Skeleton& skel, const Pose& pose, std::vector<Mat4>* out) {
  const std::size_t n = skel.joints.size();
  out->resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    const JointPose jp = i < pose.size() ? pose[i] : JointPose{};
    const Mat4 local = Mat4::trs(jp.t, jp.r, jp.s);
    const int p = skel.joints[i].parent;
    (*out)[i] = (p >= 0 && static_cast<std::size_t>(p) < i) ? (*out)[static_cast<std::size_t>(p)] * local
                                                             : skel.joints[i].base * local;
  }
}

void skin_palette(const Skeleton& skel, const Pose& pose, std::vector<Mat4>* out) {
  joint_matrices(skel, pose, out);
  if (out->size() > static_cast<std::size_t>(kMaxBones)) out->resize(kMaxBones);
  for (std::size_t i = 0; i < out->size(); ++i) (*out)[i] = (*out)[i] * skel.joints[i].inverse_bind;
}

namespace {
Mat4 blend_matrix(const std::vector<Mat4>& palette, const float joints[4], const float weights[4]) {
  float total = 0.0f;
  for (int k = 0; k < 4; ++k) total += std::max(weights[k], 0.0f);
  Mat4 m;
  for (float& f : m.m) f = 0.0f;
  if (total <= 0.0f) return Mat4{};
  for (int k = 0; k < 4; ++k) {
    const float w = std::max(weights[k], 0.0f) / total;
    if (w <= 0.0f) continue;
    const int j = static_cast<int>(joints[k]);
    const Mat4 bm = (j >= 0 && static_cast<std::size_t>(j) < palette.size()) ? palette[static_cast<std::size_t>(j)]
                                                                             : Mat4{};
    for (int i = 0; i < 16; ++i) m.m[i] += bm.m[i] * w;
  }
  return m;
}
}  // namespace

Vec3 skin_point(const std::vector<Mat4>& palette, const float joints[4], const float weights[4],
                const Vec3& p) {
  return blend_matrix(palette, joints, weights).transform_point(p);
}

Vec3 skin_dir(const std::vector<Mat4>& palette, const float joints[4], const float weights[4],
              const Vec3& d) {
  return blend_matrix(palette, joints, weights).transform_dir(d);
}

std::vector<float> blend_weights_1d(const std::vector<float>& points, float value) {
  std::vector<float> w(points.size(), 0.0f);
  if (points.empty()) return w;
  std::vector<std::size_t> order(points.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return points[a] < points[b]; });
  if (value <= points[order.front()]) {
    w[order.front()] = 1.0f;
    return w;
  }
  if (value >= points[order.back()]) {
    w[order.back()] = 1.0f;
    return w;
  }
  for (std::size_t k = 0; k + 1 < order.size(); ++k) {
    const float a = points[order[k]], b = points[order[k + 1]];
    if (value >= a && value <= b) {
      const float t = b > a ? (value - a) / (b - a) : 0.0f;
      w[order[k]] = 1.0f - t;
      w[order[k + 1]] += t;
      return w;
    }
  }
  w[order.back()] = 1.0f;
  return w;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
