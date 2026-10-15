#include "render3d/Animator.h"

#include <algorithm>
#include <cmath>

namespace ts {
namespace tombstone {
namespace render3d {

using json::Value;
using scene3d::AnimLayer3D;
using scene3d::AnimParam3D;
using scene3d::AnimState3D;
using scene3d::AnimTransition3D;
using scene3d::Animator3D;

// ---- JSON (scene.json v12 "animator") --------------------------------------------

namespace {

const char* op_name(AnimTransition3D::Op op) {
  switch (op) {
    case AnimTransition3D::Op::Greater: return ">";
    case AnimTransition3D::Op::Less: return "<";
    case AnimTransition3D::Op::True: return "true";
    case AnimTransition3D::Op::False: return "false";
    case AnimTransition3D::Op::Trigger: return "trigger";
    case AnimTransition3D::Op::Always: return "always";
  }
  return ">";
}

AnimTransition3D::Op op_from(const std::string& s) {
  if (s == "<") return AnimTransition3D::Op::Less;
  if (s == "true") return AnimTransition3D::Op::True;
  if (s == "false") return AnimTransition3D::Op::False;
  if (s == "trigger") return AnimTransition3D::Op::Trigger;
  if (s == "always") return AnimTransition3D::Op::Always;
  return AnimTransition3D::Op::Greater;
}

Value strings(const std::vector<std::string>& v) {
  Value a = Value::array();
  for (const std::string& s : v) a.push(Value::string(s));
  return a;
}

float finite(float v, float fallback, float lo, float hi) {
  if (!std::isfinite(v)) return fallback;
  return std::clamp(v, lo, hi);
}

}  // namespace

Value animator_to_json(const Animator3D& a) {
  Value o = Value::object();
  if (!a.model.empty()) o.set("model", Value::string(a.model));
  if (a.speed != 1.0f) o.set("speed", Value::number(a.speed));
  if (a.root_motion) o.set("root_motion", Value::boolean(true));
  if (!a.root_bone.empty()) o.set("root_bone", Value::string(a.root_bone));
  if (!a.drive_from_player) o.set("drive_from_player", Value::boolean(false));
  Value params = Value::array();
  for (const AnimParam3D& p : a.params) {
    Value v = Value::object();
    v.set("name", Value::string(p.name));
    v.set("type", Value::string(p.type == AnimParam3D::Type::Bool      ? "bool"
                                : p.type == AnimParam3D::Type::Trigger ? "trigger"
                                                                       : "float"));
    if (p.type == AnimParam3D::Type::Float) v.set("value", Value::number(p.value));
    else if (p.type == AnimParam3D::Type::Bool) v.set("value", Value::boolean(p.value > 0.5f));
    params.push(std::move(v));
  }
  o.set("params", std::move(params));
  Value layers = Value::array();
  for (const AnimLayer3D& l : a.layers) {
    Value lv = Value::object();
    lv.set("name", Value::string(l.name));
    if (!l.mask.empty()) lv.set("mask", strings(l.mask));
    if (l.weight != 1.0f) lv.set("weight", Value::number(l.weight));
    if (!l.entry.empty()) lv.set("entry", Value::string(l.entry));
    Value states = Value::array();
    for (const AnimState3D& s : l.states) {
      Value sv = Value::object();
      sv.set("name", Value::string(s.name));
      if (!s.clip.empty()) sv.set("clip", Value::string(s.clip));
      if (!s.blend.empty()) {
        Value b = Value::array();
        for (const auto& p : s.blend) {
          Value pv = Value::object();
          pv.set("clip", Value::string(p.clip));
          pv.set("at", Value::number(p.at));
          b.push(std::move(pv));
        }
        sv.set("blend", std::move(b));
        sv.set("blend_param", Value::string(s.blend_param));
      }
      if (s.speed != 1.0f) sv.set("speed", Value::number(s.speed));
      if (!s.loop) sv.set("loop", Value::boolean(false));
      states.push(std::move(sv));
    }
    lv.set("states", std::move(states));
    Value trans = Value::array();
    for (const AnimTransition3D& t : l.transitions) {
      Value tv = Value::object();
      tv.set("from", Value::string(t.from));
      tv.set("to", Value::string(t.to));
      if (!t.param.empty()) tv.set("param", Value::string(t.param));
      tv.set("op", Value::string(op_name(t.op)));
      if (t.op == AnimTransition3D::Op::Greater || t.op == AnimTransition3D::Op::Less) {
        tv.set("value", Value::number(t.value));
      }
      tv.set("duration", Value::number(t.duration));
      if (t.at_end) tv.set("at_end", Value::boolean(true));
      trans.push(std::move(tv));
    }
    if (trans.size() > 0) lv.set("transitions", std::move(trans));
    layers.push(std::move(lv));
  }
  o.set("layers", std::move(layers));
  if (!a.events.empty()) {
    Value ev = Value::array();
    for (const auto& e : a.events) {
      Value v = Value::object();
      v.set("clip", Value::string(e.clip));
      v.set("time", Value::number(e.time));
      v.set("name", Value::string(e.name));
      ev.push(std::move(v));
    }
    o.set("events", std::move(ev));
  }
  return o;
}

Animator3D animator_from_json(const Value& o) {
  Animator3D a;
  a.model = o.str("model");
  a.speed = finite(o.flt("speed", 1.0f), 1.0f, 0.0f, 10.0f);
  a.root_motion = o.flag("root_motion", false);
  a.root_bone = o.str("root_bone");
  a.drive_from_player = o.flag("drive_from_player", true);
  for (const Value& v : o["params"].items()) {
    AnimParam3D p;
    p.name = v.str("name");
    if (p.name.empty()) continue;
    const std::string t = v.str("type", "float");
    p.type = t == "bool" ? AnimParam3D::Type::Bool : t == "trigger" ? AnimParam3D::Type::Trigger
                                                                    : AnimParam3D::Type::Float;
    if (p.type == AnimParam3D::Type::Float) p.value = finite(v.flt("value", 0.0f), 0.0f, -1e6f, 1e6f);
    else if (p.type == AnimParam3D::Type::Bool) p.value = v.flag("value", false) ? 1.0f : 0.0f;
    a.params.push_back(p);
  }
  for (const Value& lv : o["layers"].items()) {
    AnimLayer3D l;
    l.name = lv.str("name", "Layer");
    for (const Value& m : lv["mask"].items()) {
      if (m.is_string()) l.mask.push_back(m.as_string());
    }
    l.weight = finite(lv.flt("weight", 1.0f), 1.0f, 0.0f, 1.0f);
    l.entry = lv.str("entry");
    for (const Value& sv : lv["states"].items()) {
      AnimState3D s;
      s.name = sv.str("name");
      s.clip = sv.str("clip");
      for (const Value& pv : sv["blend"].items()) {
        s.blend.push_back({pv.str("clip"), finite(pv.flt("at", 0.0f), 0.0f, -1e6f, 1e6f)});
      }
      s.blend_param = sv.str("blend_param");
      s.speed = finite(sv.flt("speed", 1.0f), 1.0f, 0.0f, 10.0f);
      s.loop = sv.flag("loop", true);
      l.states.push_back(std::move(s));
    }
    for (const Value& tv : lv["transitions"].items()) {
      AnimTransition3D t;
      t.from = tv.str("from", "*");
      t.to = tv.str("to");
      t.param = tv.str("param");
      t.op = op_from(tv.str("op", ">"));
      t.value = finite(tv.flt("value", 0.0f), 0.0f, -1e6f, 1e6f);
      t.duration = finite(tv.flt("duration", 0.2f), 0.2f, 0.0f, 10.0f);
      t.at_end = tv.flag("at_end", false);
      l.transitions.push_back(std::move(t));
    }
    a.layers.push_back(std::move(l));
  }
  for (const Value& v : o["events"].items()) {
    scene3d::AnimEvent3D e;
    e.clip = v.str("clip");
    e.time = finite(v.flt("time", 0.0f), 0.0f, 0.0f, 1e6f);
    e.name = v.str("name");
    if (!e.name.empty()) a.events.push_back(std::move(e));
  }
  return a;
}

}  // namespace render3d

namespace scene3d {

Animator3D default_locomotion_animator() {
  Animator3D a;
  a.params = {{"speed", AnimParam3D::Type::Float, 0.0f},
              {"grounded", AnimParam3D::Type::Bool, 1.0f},
              {"jump", AnimParam3D::Type::Trigger, 0.0f},
              {"vertical_speed", AnimParam3D::Type::Float, 0.0f}};
  AnimLayer3D base;
  base.name = "Base";
  base.entry = "locomotion";
  AnimState3D loco;
  loco.name = "locomotion";
  loco.blend = {{"idle", 0.0f}, {"walk", 1.8f}, {"run", 4.5f}};
  loco.blend_param = "speed";
  AnimState3D jump;
  jump.name = "jump";
  jump.clip = "jump";
  jump.loop = false;
  base.states = {loco, jump};
  AnimTransition3D to_jump;
  to_jump.from = "*";
  to_jump.to = "jump";
  to_jump.param = "jump";
  to_jump.op = AnimTransition3D::Op::Trigger;
  to_jump.duration = 0.1f;
  AnimTransition3D land;
  land.from = "jump";
  land.to = "locomotion";
  land.param = "grounded";
  land.op = AnimTransition3D::Op::True;
  land.duration = 0.25f;
  land.at_end = true;
  base.transitions = {to_jump, land};
  AnimLayer3D upper;
  upper.name = "Upper Body";
  upper.mask = {"chest"};
  a.layers = {base, upper};
  a.events = {{"walk", 0.0f, "footstep"}, {"walk", 0.5f, "footstep"},
              {"run", 0.0f, "footstep"}, {"run", 0.3f, "footstep"}};
  return a;
}

}  // namespace scene3d

namespace render3d {

// ---- Runtime ------------------------------------------------------------------------

bool AnimatorRuntime::bind(const Animator3D& cfg, const Model* model, std::string* problems) {
  cfg_ = cfg;
  model_ = model;
  layers_.clear();
  params_.clear();
  fired_.clear();
  root_motion_ = {};
  if (cfg_.layers.empty()) cfg_.layers.push_back(AnimLayer3D{});
  for (const AnimParam3D& p : cfg_.params) params_.push_back({p.name, p.type, p.value});
  if (!model_ || !model_->skinned()) {
    pose_.clear();
    if (problems) *problems = model_ ? "the model has no skin" : "no model";
    return false;
  }
  const Skeleton& sk = model_->skeleton;
  pose_ = rest_pose(sk);
  root_ = cfg_.root_bone.empty() ? 0 : std::max(0, sk.find(cfg_.root_bone));
  root_rest_ = sk.joints[static_cast<std::size_t>(root_)].t;
  std::string missing;
  auto need = [&](const std::string& clip) {
    if (!clip.empty() && clip_index(clip) < 0 && missing.find("'" + clip + "'") == std::string::npos) {
      missing += (missing.empty() ? "" : ", ") + ("'" + clip + "'");
    }
  };
  layers_.resize(cfg_.layers.size());
  for (std::size_t l = 0; l < cfg_.layers.size(); ++l) {
    const AnimLayer3D& lc = cfg_.layers[l];
    Layer& L = layers_[l];
    L.mask = l == 0 ? std::vector<float>(sk.joints.size(), 1.0f) : sk.mask_for(lc.mask);
    for (const AnimState3D& s : lc.states) {
      need(s.clip);
      for (const auto& p : s.blend) need(p.clip);
    }
    for (std::size_t s = 0; s < lc.states.size(); ++s) {
      if (lc.states[s].name == lc.entry) L.entry = static_cast<int>(s);
    }
    if (L.entry < 0 && !lc.states.empty()) L.entry = 0;
    if (L.entry >= 0) {
      L.cur.state = L.entry;
      L.alpha = L.alpha_target = 1.0f;
    }
    if (l == 0) L.alpha = L.alpha_target = 1.0f;
  }
  for (const auto& e : cfg_.events) need(e.clip);
  if (problems) *problems = missing.empty() ? "" : "clips not in the model: " + missing;
  update(0.0f);
  fired_.clear();
  root_motion_ = {};
  return true;
}

int AnimatorRuntime::clip_index(const std::string& name) const {
  return model_ ? model_->find_clip(name) : -1;
}

AnimatorRuntime::Param* AnimatorRuntime::find_param(const std::string& name) {
  for (Param& p : params_) {
    if (p.name == name) return &p;
  }
  return nullptr;
}
const AnimatorRuntime::Param* AnimatorRuntime::find_param(const std::string& name) const {
  for (const Param& p : params_) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

void AnimatorRuntime::set_float(const std::string& name, float v) {
  if (!std::isfinite(v)) return;
  if (Param* p = find_param(name)) p->value = v;
  else if (!name.empty() && params_.size() < 256) params_.push_back({name, AnimParam3D::Type::Float, v});
}
void AnimatorRuntime::set_bool(const std::string& name, bool v) {
  if (Param* p = find_param(name)) p->value = v ? 1.0f : 0.0f;
  else if (!name.empty() && params_.size() < 256) params_.push_back({name, AnimParam3D::Type::Bool, v ? 1.0f : 0.0f});
}
void AnimatorRuntime::trigger(const std::string& name) {
  if (Param* p = find_param(name)) p->value = 1.0f;
  else if (!name.empty() && params_.size() < 256) params_.push_back({name, AnimParam3D::Type::Trigger, 1.0f});
}
void AnimatorRuntime::set_param(const std::string& name, float v) {
  const Param* p = find_param(name);
  if (p && p->type != AnimParam3D::Type::Float) {
    set_bool(name, v > 0.5f);
  } else {
    set_float(name, v);
  }
}
float AnimatorRuntime::param(const std::string& name) const {
  const Param* p = find_param(name);
  return p ? p->value : 0.0f;
}
bool AnimatorRuntime::has_param(const std::string& name) const { return find_param(name) != nullptr; }

void AnimatorRuntime::weights_of(int layer, const Playing& p,
                                 std::vector<std::pair<int, float>>* out) const {
  out->clear();
  if (!model_) return;
  if (p.state < 0) {
    if (p.clip >= 0) out->push_back({p.clip, 1.0f});
    return;
  }
  const AnimState3D& s = cfg_.layers[static_cast<std::size_t>(layer)].states[static_cast<std::size_t>(p.state)];
  if (s.blend.empty()) {
    const int c = clip_index(s.clip);
    if (c >= 0) out->push_back({c, 1.0f});
    return;
  }
  std::vector<float> at;
  std::vector<int> clips;
  for (const auto& b : s.blend) {
    const int c = clip_index(b.clip);
    if (c < 0) continue;
    at.push_back(b.at);
    clips.push_back(c);
  }
  const std::vector<float> w = blend_weights_1d(at, param(s.blend_param));
  for (std::size_t i = 0; i < w.size(); ++i) {
    if (w[i] > 1e-4f) out->push_back({clips[i], w[i]});
  }
}

float AnimatorRuntime::duration_of(int layer, const Playing& p) const {
  std::vector<std::pair<int, float>> w;
  weights_of(layer, p, &w);
  float d = 0.0f;
  for (const auto& [c, k] : w) d += model_->clips[static_cast<std::size_t>(c)].duration * k;
  return std::max(d, 1e-3f);
}

bool AnimatorRuntime::looping(int layer, const Playing& p) const {
  if (p.state < 0) return false;
  return cfg_.layers[static_cast<std::size_t>(layer)].states[static_cast<std::size_t>(p.state)].loop;
}

void AnimatorRuntime::eval(int layer, const Playing& p, Pose* pose) const {
  std::vector<std::pair<int, float>> w;
  weights_of(layer, p, &w);
  if (w.empty()) return;
  const bool loop = looping(layer, p);
  auto sample = [&](int c, Pose* into) {
    const Clip& clip = model_->clips[static_cast<std::size_t>(c)];
    sample_clip(clip, p.phase * clip.duration, loop, into);
  };
  if (w.size() == 1) {
    sample(w[0].first, pose);
    return;
  }
  const Pose base = *pose;
  sample(w[0].first, pose);
  float total = w[0].second;
  for (std::size_t i = 1; i < w.size(); ++i) {
    Pose other = base;
    sample(w[i].first, &other);
    total += w[i].second;
    blend_poses(*pose, other, w[i].second / total, pose);
  }
}

void AnimatorRuntime::advance(int layer, Playing* p, float dt, bool fire) {
  if (!p->active()) return;
  float speed = cfg_.speed;
  if (p->state >= 0) speed *= cfg_.layers[static_cast<std::size_t>(layer)].states[static_cast<std::size_t>(p->state)].speed;
  const float old = p->phase;
  const float d = duration_of(layer, *p);
  p->phase += dt * speed / d;
  const bool loop = looping(layer, *p);
  if (!loop) p->phase = std::min(p->phase, 1.0f);
  if (!fire || cfg_.events.empty() || p->phase <= old) return;
  // Events of the dominant clip.
  std::vector<std::pair<int, float>> w;
  weights_of(layer, *p, &w);
  if (w.empty()) return;
  const auto dom = *std::max_element(w.begin(), w.end(),
                                     [](const auto& a, const auto& b) { return a.second < b.second; });
  const Clip& clip = model_->clips[static_cast<std::size_t>(dom.first)];
  const float cd = std::max(clip.duration, 1e-3f);
  for (const auto& e : cfg_.events) {
    if (e.clip != clip.name) continue;
    const float at = std::clamp(e.time / cd, 0.0f, 1.0f);
    if (loop) {
      for (float k = std::floor(old); k <= std::floor(p->phase); k += 1.0f) {
        const float x = k + at;
        if (x > old && x <= p->phase && fired_.size() < 64) fired_.push_back({e.name, clip.name, e.time});
      }
    } else if (at > old && at <= p->phase && fired_.size() < 64) {
      fired_.push_back({e.name, clip.name, e.time});
    } else if (old == 0.0f && at == 0.0f && fired_.size() < 64) {
      fired_.push_back({e.name, clip.name, e.time});
    }
  }
}

void AnimatorRuntime::go(int layer, const Playing& to, float fade) {
  Layer& L = layers_[static_cast<std::size_t>(layer)];
  Playing next = to;
  next.phase = 0.0f;
  if (L.cur.active() && L.alpha > 0.0f && fade > 0.0f) {
    L.prev = L.cur;
    L.fade = 0.0f;
    L.fade_time = fade;
  } else {
    L.prev = {};
    L.fade = 1.0f;
    L.fade_time = 0.0f;
  }
  if (layer > 0 && L.alpha <= 0.0f) {
    L.alpha_rate = fade > 0.0f ? 1.0f / fade : 1000.0f;
  }
  L.alpha_target = 1.0f;
  L.cur = next;
}

void AnimatorRuntime::transitions(int layer) {
  Layer& L = layers_[static_cast<std::size_t>(layer)];
  const AnimLayer3D& lc = cfg_.layers[static_cast<std::size_t>(layer)];
  const std::string cur = L.cur.state >= 0 ? lc.states[static_cast<std::size_t>(L.cur.state)].name : "";
  const bool finished = L.cur.active() && !looping(layer, L.cur) && L.cur.phase >= 1.0f;
  for (const AnimTransition3D& t : lc.transitions) {
    if (t.from != "*" && t.from != cur) continue;
    if (t.to == cur && L.cur.state >= 0) continue;
    if (t.from != "*" && L.cur.state < 0) continue;
    if (t.at_end && !finished) continue;
    int target = -1;
    for (std::size_t s = 0; s < lc.states.size(); ++s) {
      if (lc.states[s].name == t.to) target = static_cast<int>(s);
    }
    if (target < 0) continue;
    const float v = param(t.param);
    bool ok = false;
    switch (t.op) {
      case AnimTransition3D::Op::Greater: ok = v > t.value; break;
      case AnimTransition3D::Op::Less: ok = v < t.value; break;
      case AnimTransition3D::Op::True: ok = v > 0.5f; break;
      case AnimTransition3D::Op::False: ok = v <= 0.5f; break;
      case AnimTransition3D::Op::Trigger: ok = v > 0.5f; break;
      case AnimTransition3D::Op::Always: ok = true; break;
    }
    if (!ok) continue;
    if (t.op == AnimTransition3D::Op::Trigger) {
      if (Param* p = find_param(t.param)) p->value = 0.0f;
    }
    Playing to;
    to.state = target;
    go(layer, to, t.duration);
    return;
  }
}

Vec3 AnimatorRuntime::root_at(const Playing& p) const {
  std::vector<std::pair<int, float>> w;
  weights_of(0, p, &w);
  Vec3 r = root_rest_;
  if (w.empty()) return r;
  r = {};
  for (const auto& [c, k] : w) {
    const Clip& clip = model_->clips[static_cast<std::size_t>(c)];
    Vec3 t = root_rest_;
    for (const Channel& ch : clip.channels) {
      if (ch.joint != root_ || ch.path != ChannelPath::Translation) continue;
      float v[4];
      float ph = p.phase;
      if (looping(0, p)) ph -= std::floor(ph);
      sample_channel(ch, std::clamp(ph, 0.0f, 1.0f) * clip.duration, v);
      t = {v[0], v[1], v[2]};
    }
    r += t * k;
  }
  return r;
}

void AnimatorRuntime::update(float dt) {
  if (!bound()) return;
  dt = std::max(0.0f, std::isfinite(dt) ? dt : 0.0f);
  const Skeleton& sk = model_->skeleton;
  for (std::size_t l = 0; l < layers_.size(); ++l) {
    const int li = static_cast<int>(l);
    Layer& L = layers_[l];
    transitions(li);
    // A one-shot clip that finished: back to the entry state, or let go.
    if (L.cur.state < 0 && L.cur.clip >= 0 && !L.cur.hold && L.cur.phase >= 1.0f) {
      if (L.entry >= 0) {
        Playing to;
        to.state = L.entry;
        go(li, to, 0.2f);
      } else {
        stop_layer(li, 0.2f);
      }
    }
    // Root motion (base layer): travel of the root between phases.
    Vec3 r0;
    const bool rm = l == 0 && cfg_.root_motion && L.cur.active();
    if (rm) r0 = root_at(L.cur);
    const float before = L.cur.phase;
    advance(li, &L.cur, dt, true);
    if (rm) {
      Vec3 r1 = root_at(L.cur);
      const float wraps = looping(0, L.cur) ? std::floor(L.cur.phase) - std::floor(before) : 0.0f;
      if (wraps > 0.0f) {
        Playing a = L.cur, b = L.cur;
        a.phase = std::floor(L.cur.phase) - 1e-4f;
        b.phase = std::floor(L.cur.phase);
        r1 += (root_at(a) - root_at(b)) * wraps;
      }
      const Vec3 d = r1 - r0;
      root_motion_ += Vec3{d.x, 0.0f, d.z};
    }
    advance(li, &L.prev, dt, false);
    if (L.fade < 1.0f) {
      L.fade = L.fade_time > 0.0f ? std::min(1.0f, L.fade + dt / L.fade_time) : 1.0f;
      if (L.fade >= 1.0f) L.prev = {};
    }
    if (l == 0) {
      L.alpha = 1.0f;
    } else if (L.alpha != L.alpha_target) {
      const float step = dt * L.alpha_rate;
      L.alpha = L.alpha < L.alpha_target ? std::min(L.alpha_target, L.alpha + step)
                                         : std::max(L.alpha_target, L.alpha - step);
      if (dt == 0.0f && L.alpha_rate >= 1000.0f) L.alpha = L.alpha_target;
      if (L.alpha <= 0.0f && L.alpha_target <= 0.0f) {
        L.cur = {};
        L.prev = {};
      }
    }
  }
  // Compose.
  pose_ = rest_pose(sk);
  for (std::size_t l = 0; l < layers_.size(); ++l) {
    const int li = static_cast<int>(l);
    const Layer& L = layers_[l];
    if (!L.cur.active() && !L.prev.active()) continue;
    if (l > 0 && L.alpha <= 0.0f) continue;
    Pose cur = pose_;
    eval(li, L.cur, &cur);
    if (L.prev.active() && L.fade < 1.0f) {
      Pose prev = pose_;
      eval(li, L.prev, &prev);
      blend_poses(prev, cur, L.fade, &cur);
    }
    if (l == 0) {
      pose_ = std::move(cur);
    } else {
      const float k = L.alpha * cfg_.layers[l].weight;
      blend_poses(pose_, cur, k, &pose_, &L.mask);
    }
  }
  if (cfg_.root_motion && static_cast<std::size_t>(root_) < pose_.size()) {
    pose_[static_cast<std::size_t>(root_)].t.x = root_rest_.x;
    pose_[static_cast<std::size_t>(root_)].t.z = root_rest_.z;
  }
}

bool AnimatorRuntime::play(const std::string& name, float fade, bool hold) {
  if (!bound() || name.empty()) return false;
  if (fade < 0.0f) fade = 0.2f;
  for (std::size_t l = 0; l < cfg_.layers.size(); ++l) {
    const auto& states = cfg_.layers[l].states;
    for (std::size_t s = 0; s < states.size(); ++s) {
      if (states[s].name != name) continue;
      Playing to;
      to.state = static_cast<int>(s);
      go(static_cast<int>(l), to, fade);
      return true;
    }
  }
  const int c = clip_index(name);
  if (c < 0) return false;
  // The top layer whose mask covers every bone the clip moves.
  int layer = 0;
  const Clip& clip = model_->clips[static_cast<std::size_t>(c)];
  for (int l = static_cast<int>(layers_.size()) - 1; l > 0; --l) {
    bool covers = true;
    for (const Channel& ch : clip.channels) {
      const auto& m = layers_[static_cast<std::size_t>(l)].mask;
      if (ch.joint < 0 || static_cast<std::size_t>(ch.joint) >= m.size() || m[static_cast<std::size_t>(ch.joint)] <= 0.0f) {
        covers = false;
        break;
      }
    }
    if (covers) {
      layer = l;
      break;
    }
  }
  Playing to;
  to.clip = c;
  to.hold = hold;
  go(layer, to, fade);
  return true;
}

void AnimatorRuntime::stop_layer(int layer, float fade) {
  if (layer < 0 || static_cast<std::size_t>(layer) >= layers_.size()) return;
  Layer& L = layers_[static_cast<std::size_t>(layer)];
  if (layer == 0) {
    Playing to;
    to.state = L.entry;
    if (L.entry >= 0) go(0, to, fade);
    else L.cur = L.prev = {};
    return;
  }
  L.alpha_target = 0.0f;
  L.alpha_rate = fade > 0.0f ? 1.0f / fade : 1000.0f;
  if (fade <= 0.0f) {
    L.alpha = 0.0f;
    L.cur = L.prev = {};
  }
}

void AnimatorRuntime::palette(std::vector<Mat4>* out) const {
  if (!bound()) {
    out->clear();
    return;
  }
  skin_palette(model_->skeleton, pose_, out);
}

std::vector<AnimatorRuntime::Fired> AnimatorRuntime::take_events() {
  std::vector<Fired> out;
  out.swap(fired_);
  return out;
}

Vec3 AnimatorRuntime::take_root_motion() {
  const Vec3 r = root_motion_;
  root_motion_ = {};
  return r;
}

std::string AnimatorRuntime::state(int layer) const {
  if (layer < 0 || static_cast<std::size_t>(layer) >= layers_.size() || !model_) return "";
  const Playing& p = layers_[static_cast<std::size_t>(layer)].cur;
  if (p.state >= 0) return cfg_.layers[static_cast<std::size_t>(layer)].states[static_cast<std::size_t>(p.state)].name;
  if (p.clip >= 0) return "clip:" + model_->clips[static_cast<std::size_t>(p.clip)].name;
  return "";
}

float AnimatorRuntime::state_phase(int layer) const {
  if (layer < 0 || static_cast<std::size_t>(layer) >= layers_.size()) return 0.0f;
  return layers_[static_cast<std::size_t>(layer)].cur.phase;
}

float AnimatorRuntime::fade_progress(int layer) const {
  if (layer < 0 || static_cast<std::size_t>(layer) >= layers_.size()) return 1.0f;
  return layers_[static_cast<std::size_t>(layer)].fade;
}

float AnimatorRuntime::layer_alpha(int layer) const {
  if (layer < 0 || static_cast<std::size_t>(layer) >= layers_.size()) return 0.0f;
  return layers_[static_cast<std::size_t>(layer)].alpha;
}

std::vector<std::pair<std::string, float>> AnimatorRuntime::state_weights(int layer) const {
  std::vector<std::pair<std::string, float>> out;
  if (layer < 0 || static_cast<std::size_t>(layer) >= layers_.size() || !model_) return out;
  std::vector<std::pair<int, float>> w;
  weights_of(layer, layers_[static_cast<std::size_t>(layer)].cur, &w);
  for (const auto& [c, k] : w) out.push_back({model_->clips[static_cast<std::size_t>(c)].name, k});
  return out;
}

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
