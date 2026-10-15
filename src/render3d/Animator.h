#pragma once

// Animator runtime: plays a scene3d::Animator3D against a skinned Model.
// Per layer a current and a previous state (crossfade), clip or 1D blend
// space states (clips kept in step by normalised time), parameter-driven
// transitions, ad-hoc clips from scripts (play / crossfade), per-bone
// masks for upper layers, clip events and optional root motion. Pure CPU:
// World3D drives one per animated entity, the editor's preview another.

#include "render3d/Gltf.h"
#include "render3d/Json.h"
#include "render3d/Scene3D.h"
#include "render3d/Skeleton.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

json::Value animator_to_json(const scene3d::Animator3D& a);
scene3d::Animator3D animator_from_json(const json::Value& v);

class AnimatorRuntime {
 public:
  struct Fired {
    std::string name;
    std::string clip;
    float time = 0.0f;
  };

  // `model` must outlive the runtime (or the next bind). Unknown clip names
  // are skipped and listed in *problems.
  bool bind(const scene3d::Animator3D& cfg, const Model* model, std::string* problems = nullptr);
  bool bound() const { return model_ != nullptr && model_->skinned(); }
  const Model* model() const { return model_; }
  const scene3d::Animator3D& config() const { return cfg_; }

  // Parameters (unknown names are added as floats / bools on first set).
  void set_float(const std::string& name, float v);
  void set_bool(const std::string& name, bool v);
  void trigger(const std::string& name);
  void set_param(const std::string& name, float v);  // by the param's own type
  float param(const std::string& name) const;
  bool has_param(const std::string& name) const;

  // Go to a state (any layer that has it) or play a clip once (on the top
  // layer whose mask covers the clip's bones; `hold` keeps its last frame,
  // else the layer returns to its entry state). fade = crossfade seconds
  // (< 0: the default 0.2). False when nothing has that name.
  bool play(const std::string& state_or_clip, float fade = -1.0f, bool hold = false);
  // Stop an ad-hoc clip / leave a layer (upper layers fade out).
  void stop_layer(int layer, float fade = 0.2f);

  void update(float dt);
  // Rest pose, posed by the last update().
  const Pose& pose() const { return pose_; }
  void palette(std::vector<Mat4>* out) const;
  std::vector<Fired> take_events();
  // Root travel since the last call (model space, ground plane only).
  Vec3 take_root_motion();

  std::size_t layer_count() const { return layers_.size(); }
  // Current state ("" = none; an ad-hoc clip shows as "clip:<name>").
  std::string state(int layer = 0) const;
  float state_phase(int layer = 0) const;  // normalised (loops count up)
  float fade_progress(int layer = 0) const;  // 1 = settled
  float layer_alpha(int layer) const;
  // Blend weights of the current state's clips (one for a plain clip).
  std::vector<std::pair<std::string, float>> state_weights(int layer = 0) const;

 private:
  struct Playing {
    int state = -1;    // index into the layer's states
    int clip = -1;     // ad-hoc clip (state == -1)
    bool hold = false;
    float phase = 0.0f;  // normalised time
    bool active() const { return state >= 0 || clip >= 0; }
  };
  struct Layer {
    Playing cur, prev;
    float fade = 1.0f, fade_time = 0.0f;
    float alpha = 0.0f, alpha_target = 0.0f, alpha_rate = 5.0f;
    std::vector<float> mask;
    int entry = -1;
  };
  struct Param {
    std::string name;
    scene3d::AnimParam3D::Type type;
    float value;
  };

  int clip_index(const std::string& name) const;
  float duration_of(int layer, const Playing& p) const;
  bool looping(int layer, const Playing& p) const;
  void weights_of(int layer, const Playing& p, std::vector<std::pair<int, float>>* out) const;
  void eval(int layer, const Playing& p, Pose* pose) const;
  void advance(int layer, Playing* p, float dt, bool fire);
  void go(int layer, const Playing& to, float fade);
  void transitions(int layer);
  Param* find_param(const std::string& name);
  const Param* find_param(const std::string& name) const;
  Vec3 root_at(const Playing& p) const;

  scene3d::Animator3D cfg_;
  const Model* model_ = nullptr;
  std::vector<Layer> layers_;
  std::vector<Param> params_;
  Pose pose_;
  std::vector<Fired> fired_;
  int root_ = 0;
  Vec3 root_rest_;
  Vec3 root_motion_;
};

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
