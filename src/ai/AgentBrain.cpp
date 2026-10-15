#include "ai/AgentBrain.h"

#include <cmath>

namespace ts {
namespace ai {

AgentBrain::AgentBrain(AgentBody* body, AgentParams params, PerceptionConfig senses)
    : body_(body), params_(params) {
  perception_.config = senses;
  if (body_) home_ = body_->position();
}

void AgentBrain::set_tree(std::shared_ptr<const BtTree> tree, std::uint64_t seed) {
  if (bt_) bt_->reset(this);
  bt_.reset();
  if (tree) {
    bt_ = std::make_unique<BtInstance>(std::move(tree), seed);
    for (const auto& [k, v] : bb_.values()) bt_->blackboard().set(k, v);
  }
}

bool AgentBrain::move_to(const V3& goal) {
  path_.clear();
  next_ = 0;
  if (!body_) return false;
  std::vector<V3> pts;
  if (!body_->plan(body_->position(), goal, &pts) || pts.empty()) return false;
  path_ = std::move(pts);
  // Skip waypoints we already stand on.
  while (next_ + 1 < path_.size() && distance_xz(path_[next_], body_->position()) <= arrive_radius()) ++next_;
  return true;
}

void AgentBrain::stop() {
  path_.clear();
  next_ = 0;
  direct_ = false;
  if (body_) body_->desire({});
}

void AgentBrain::follow() {
  if (!body_) return;
  const V3 at = body_->position();
  while (next_ < path_.size() && distance_xz(path_[next_], at) <= arrive_radius()) ++next_;
  if (next_ >= path_.size()) {
    path_.clear();
    next_ = 0;
    direct_ = false;
    body_->desire({});
    return;
  }
  V3 d = flat(path_[next_] - at);
  const float len = length_xz(d);
  float speed = params_.speed;
  // Ease into the final point instead of overshooting it.
  if (next_ + 1 == path_.size() && len < params_.speed * 0.25f) speed = std::max(len * 4.0f, params_.speed * 0.2f);
  body_->desire(len > 1e-6f ? d * (speed / len) : V3{});
}

void AgentBrain::tick(float dt, double now) {
  now_ = now;
  perception_.forget(now);
  if (bt_ && !direct_) bt_->tick(dt, *this);
  follow();
}

void AgentBrain::see(const std::vector<Perception::Candidate>& candidates, const LineClearFn& clear, double now) {
  if (!body_) return;
  perception_.see(body_->position(), body_->forward(), candidates, clear, now);
}

bool AgentBrain::hear(std::uint64_t source, const V3& at, float loudness, const std::string& what, double now) {
  if (!body_) return false;
  const bool heard = perception_.hear(source, at, loudness, what, body_->position(), now);
  if (heard) {
    Blackboard& bb = blackboard();
    bb.set_vec("noise_pos", at);
    bb.set_text("noise", what);
    bb.set_number("noise_time", now);
  }
  return heard;
}

bool AgentBrain::resolve(const std::string& key, V3* out, std::uint64_t* id) const {
  if (id) *id = 0;
  const Blackboard& bb = blackboard();
  const BbValue* v = bb.get(key);
  if (!v) {
    // Not on the blackboard: an entity name.
    if (!body_) return false;
    const std::uint64_t e = body_->find(key);
    if (!e) return false;
    if (id) *id = e;
    return body_->entity_pos(e, out);
  }
  if (bb.vec(key, out)) return true;
  if (v->type == BbValue::Type::Number) {
    const auto e = static_cast<std::uint64_t>(v->number());
    if (id) *id = e;
    // Prefer what we perceive (last known), else the live position.
    if (const Percept* p = perception_.find(e)) {
      *out = p->pos;
      if (p->visible) return true;
    }
    return body_ && e && body_->entity_pos(e, out);
  }
  if (v->type == BbValue::Type::Text && body_) {
    const std::uint64_t e = body_->find(v->str());
    if (!e) return false;
    if (id) *id = e;
    return body_->entity_pos(e, out);
  }
  return false;
}

BtStatus AgentBrain::action(BtContext& ctx) {
  const BtNode& n = *ctx.node;
  const std::string& t = n.type;
  if (!body_) return BtStatus::Failure;
  const V3 at = body_->position();
  if (t == "move_to") {
    V3 goal;
    if (ctx.entering) {
      if (!resolve(n.text("key", "target"), &goal)) return BtStatus::Failure;
      const float s = static_cast<float>(n.num("speed", 0.0));
      if (s > 0.0f) ctx.counter = 1, ctx.point = goal;
      if (!move_to(goal)) return BtStatus::Failure;
    }
    return moving() ? BtStatus::Running : BtStatus::Success;
  }
  if (t == "wander") {
    if (ctx.entering) {
      const float r = static_cast<float>(n.num("radius", params_.radius * 12.0f));
      V3 p;
      bool ok = false;
      for (int k = 0; k < 6 && !ok; ++k) {
        ok = body_->random_point(home_, r, ctx.random(), ctx.random(), &p) && move_to(p);
      }
      if (!ok) return BtStatus::Failure;
    }
    return moving() ? BtStatus::Running : BtStatus::Success;
  }
  if (t == "chase") {
    V3 goal;
    std::uint64_t id = 0;
    if (!resolve(n.text("key", "target"), &goal, &id)) return BtStatus::Failure;
    const float range = static_cast<float>(n.num("range", params_.radius * 3.0f));
    if (distance_xz(goal, at) <= range) {
      path_.clear();
      next_ = 0;
      body_->desire({});
      return BtStatus::Success;
    }
    ctx.timer -= ctx.dt;
    if (ctx.entering || ctx.timer <= 0.0 || !moving()) {
      ctx.timer = params_.replan;
      if (!move_to(goal)) return BtStatus::Failure;
    }
    return BtStatus::Running;
  }
  if (t == "flee") {
    V3 from;
    if (!resolve(n.text("key", "target"), &from)) return BtStatus::Failure;
    const float dist = static_cast<float>(n.num("distance", params_.radius * 15.0f));
    if (distance_xz(from, at) >= dist) {
      stop();
      return BtStatus::Success;
    }
    if (ctx.entering || !moving()) {
      V3 away = normalized(flat(at - from));
      if (length_xz(away) < 1e-4f) away = {1, 0, 0};
      const V3 goal = at + away * dist;
      V3 p;
      if (!move_to(goal) && !(body_->random_point(goal, dist * 0.5f, ctx.random(), ctx.random(), &p) && move_to(p))) {
        return BtStatus::Failure;
      }
    }
    return BtStatus::Running;
  }
  if (t == "attack") {
    V3 goal;
    std::uint64_t id = 0;
    if (!resolve(n.text("key", "target"), &goal, &id) || !id) return BtStatus::Failure;
    if (distance_xz(goal, at) > params_.radius * 3.0f) return BtStatus::Failure;
    return body_->attack(id, n.num("damage", 1.0)) ? BtStatus::Success : BtStatus::Failure;
  }
  if (t == "face") {
    V3 goal;
    if (!resolve(n.text("key", "target"), &goal)) return BtStatus::Failure;
    body_->desire(normalized(flat(goal - at)) * 1e-3f);
    return BtStatus::Success;
  }
  if (t == "play_anim") {
    return body_->play(n.text("clip"), static_cast<float>(n.num("fade", 0.2))) ? BtStatus::Success : BtStatus::Failure;
  }
  if (t == "stop") {
    stop();
    return BtStatus::Success;
  }
  if (t == "lua") return body_->lua(n.text("fn"), blackboard());
  return BtStatus::Failure;
}

bool AgentBrain::condition(BtContext& ctx) {
  const BtNode& n = *ctx.node;
  const std::string& t = n.type;
  if (t == "can_see") {
    const std::string key = n.text("target", n.text("key", ""));
    if (key.empty() || key == "any") {
      // Remember who: the first visible percept becomes "target".
      for (const Percept& p : perception_.known()) {
        if (p.visible && p.id) {
          blackboard().set_number("target", static_cast<double>(p.id));
          return true;
        }
      }
      return false;
    }
    V3 pos;
    std::uint64_t id = 0;
    if (!resolve(key, &pos, &id) || !id) return false;
    return perception_.sees(id);
  }
  if (t == "heard") {
    const Percept* p = perception_.last_heard(now_, n.num("within", 2.0));
    if (!p) return false;
    blackboard().set_vec(n.text("key", "noise_pos"), p->pos);
    return true;
  }
  if (t == "near") {
    V3 pos;
    if (!body_ || !resolve(n.text("key", "target"), &pos)) return false;
    return distance_xz(pos, body_->position()) <= static_cast<float>(n.num("distance", params_.radius * 3.0f));
  }
  return false;
}

void AgentBrain::halt(BtContext& ctx) {
  const std::string& t = ctx.node->type;
  if (t == "move_to" || t == "wander" || t == "chase" || t == "flee") {
    path_.clear();
    next_ = 0;
    if (body_) body_->desire({});
  }
}

void AgentBrain::log(BtContext& /*ctx*/, const std::string& text) {
  if (body_) body_->log(text);
}

}  // namespace ai
}  // namespace ts
