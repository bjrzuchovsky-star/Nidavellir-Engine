#pragma once

// AgentBrain: the behaviour-tree host every engine side shares. The 2D
// plugin and the 3D extension implement AgentBody (where am I, plan a path,
// where is X, move like this) and the brain answers the host nodes of
// bt_node_types(): move_to, wander, chase, flee, attack, face, play_anim,
// stop, lua, can_see, heard, near. Positions are 3D (y up); 2D uses (x, 0, y).

#include "ai/BehaviorTree.h"
#include "ai/Perception.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace ai {

struct AgentParams {
  float speed = 3.0f;       // units / s
  float radius = 0.4f;      // body radius (avoidance, arrival)
  float avoidance = 1.0f;   // 0 = walk through others, 1 = full separation
  float arrive = 0.0f;      // waypoint reached within (0 = radius)
  float replan = 0.5f;      // chase: seconds between path refreshes
};

class AgentBody {
 public:
  virtual ~AgentBody() = default;
  virtual V3 position() const = 0;
  virtual V3 forward() const = 0;
  // A walkable path from -> to (first point may be `from`); false = none.
  virtual bool plan(const V3& from, const V3& to, std::vector<V3>* points) = 0;
  // A reachable point within `radius` of `around` (u, v are seeded 0..1).
  virtual bool random_point(const V3& around, float radius, float u, float v, V3* out) = 0;
  virtual bool entity_pos(std::uint64_t id, V3* out) const = 0;
  virtual std::uint64_t find(const std::string& name) const = 0;
  // Desired velocity this tick (zero = stand still).
  virtual void desire(const V3& velocity) = 0;
  virtual bool attack(std::uint64_t /*target*/, double /*damage*/) { return false; }
  virtual bool play(const std::string& /*clip*/, float /*fade*/) { return false; }
  virtual BtStatus lua(const std::string& /*fn*/, Blackboard& /*bb*/) { return BtStatus::Failure; }
  virtual void log(const std::string& /*text*/) {}
};

class AgentBrain : public BtHost {
 public:
  AgentBrain(AgentBody* body, AgentParams params, PerceptionConfig senses = {});

  void set_tree(std::shared_ptr<const BtTree> tree, std::uint64_t seed);
  BtInstance* tree() { return bt_.get(); }
  const BtInstance* tree() const { return bt_.get(); }
  Blackboard& blackboard() { return bt_ ? bt_->blackboard() : bb_; }
  const Blackboard& blackboard() const { return bt_ ? bt_->blackboard() : bb_; }
  AgentParams& params() { return params_; }
  Perception& perception() { return perception_; }
  const Perception& perception() const { return perception_; }
  PerceptionConfig& senses() { return perception_.config; }
  void set_home(const V3& h) { home_ = h; }
  const V3& home() const { return home_; }

  // Direct steering (Lua ai.move_to / ai.stop): overrides the tree until arrival.
  bool move_to(const V3& goal);
  void stop();
  bool moving() const { return next_ < path_.size(); }
  const std::vector<V3>& path() const { return path_; }
  std::size_t next_waypoint() const { return next_; }

  // One tick: tree (if any), then path following -> body->desire().
  void tick(float dt, double now);
  // Sight / hearing feed (host gathers candidates).
  void see(const std::vector<Perception::Candidate>& candidates, const LineClearFn& clear, double now);
  bool hear(std::uint64_t source, const V3& at, float loudness, const std::string& what, double now);

  // BtHost
  BtStatus action(BtContext& ctx) override;
  bool condition(BtContext& ctx) override;
  void halt(BtContext& ctx) override;
  void log(BtContext& ctx, const std::string& text) override;

  // Where a blackboard key points: a vec, an entity id (number) or a name.
  bool resolve(const std::string& key, V3* out, std::uint64_t* id = nullptr) const;

 private:
  void follow();
  float arrive_radius() const { return params_.arrive > 0.0f ? params_.arrive : params_.radius; }

  AgentBody* body_;
  AgentParams params_;
  Perception perception_;
  std::unique_ptr<BtInstance> bt_;
  Blackboard bb_;
  V3 home_;
  std::vector<V3> path_;
  std::size_t next_ = 0;
  double now_ = 0.0;
  bool direct_ = false;  // a move_to from script is in charge
};

}  // namespace ai
}  // namespace ts
