#pragma once

// Data-driven behaviour trees (*.bt.json) with a blackboard, plus utility
// scoring as a composite.
//
//   { "name": "guard", "blackboard": {"patrol_speed": 2},
//     "root": { "type": "selector", "children": [
//       { "type": "sequence", "children": [
//         { "type": "can_see", "target": "player" },
//         { "type": "chase", "key": "target" } ] },
//       { "type": "wander", "radius": 6 } ] } }
//
// Composites: sequence, selector (both remember the running child; add
// "reactive": true to re-check earlier children every tick), parallel
// ("success" / "failure" thresholds), random (seeded shuffle), utility
// (children carry "utility": {"weight", "considerations": [...]}; the
// best score runs, with "hysteresis").
// Decorators: inverter, succeeder, failer, repeat ("times", 0 = forever),
// retry ("times"), cooldown ("seconds"), timeout ("seconds"), if (a
// blackboard check guarding its child every tick).
// Conditions: check ("key", "op", "value"), chance ("p").
// Actions: wait ("seconds", "max"), set ("key", "value"), inc ("key",
// "by"), log ("text"), succeed, fail, running.
// Anything else goes to the host (BtHost): move_to, wander, chase, flee,
// attack, can_see, heard, near, play_anim, lua, ...
//
// Ticking is deterministic: the same tree, seed, blackboard and host
// answers give the same statuses (the smokes trace it twice).

#include "ai/AiJson.h"
#include "ai/Blackboard.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace ai {

enum class BtStatus : std::uint8_t { Idle, Success, Failure, Running };
const char* to_string(BtStatus s);

enum class BtKind : std::uint8_t { Composite, Decorator, Condition, Action };
const char* to_string(BtKind k);

struct BtNode {
  std::string type;
  std::string name;       // label for the editor / debugger ("" = type)
  JsonValue params;       // every other field of the node object
  std::vector<int> children;
  int parent = -1;
  const JsonValue& param(const std::string& key) const { return params.get(key); }
  double num(const std::string& key, double fallback) const { return params.get(key).num(fallback); }
  std::string text(const std::string& key, const std::string& fallback = "") const {
    return params.get(key).str_or(fallback);
  }
  std::string label() const { return name.empty() ? type : name; }
};

struct BtNodeInfo {
  const char* type;
  BtKind kind;
  const char* params;  // "seconds max" (editor hints)
  const char* help;
  bool host = false;   // answered by the host, not the tree runtime
};
// Every node type the runtime and the stock hosts know (editor palette).
const std::vector<BtNodeInfo>& bt_node_types();
const BtNodeInfo* bt_node_info(const std::string& type);

struct BtTree {
  std::string name;
  std::vector<BtNode> nodes;  // nodes[0] is the root
  JsonValue blackboard;       // defaults (object)

  bool parse(const std::string& text, std::string* error);
  static bool from_json(const JsonValue& j, BtTree* out, std::string* error);
  JsonValue to_json() const;
  std::string write() const { return to_json().dump(2) + "\n"; }
  bool empty() const { return nodes.empty(); }
  // Problems that do not stop loading (unknown types, decorators without
  // a child...).
  std::vector<std::string> lint() const;
  // Editing helpers (editor): add a child under `parent` (-1 = new root);
  // remove a node and its subtree; move within its parent.
  int add(int parent, const std::string& type);
  void remove(int node);
  void move(int node, int delta);
};

class BtInstance;

// Per-tick view a host action / condition gets.
struct BtContext {
  BtInstance* bt = nullptr;
  Blackboard* bb = nullptr;
  const BtNode* node = nullptr;
  int index = -1;
  double now = 0.0;   // seconds since the tree started
  float dt = 0.0f;
  bool entering = false;  // first tick of this node's run
  // Node scratch the host may use (cleared on entry / halt).
  double timer = 0.0;
  V3 point;
  int counter = 0;
  float random();  // 0..1, deterministic per instance
};

class BtHost {
 public:
  virtual ~BtHost() = default;
  virtual BtStatus action(BtContext& /*ctx*/) { return BtStatus::Failure; }
  virtual bool condition(BtContext& /*ctx*/) { return false; }
  // A running action was abandoned (a reactive branch won, a timeout hit).
  virtual void halt(BtContext& /*ctx*/) {}
  virtual void log(BtContext& /*ctx*/, const std::string& /*text*/) {}
};

// Utility scoring: weight * product of curve(input) over considerations.
//   {"key": "hunger", "curve": "linear|quadratic|sqrt|inverse|logistic|step",
//    "min": 0, "max": 100, "threshold": 0.5}
float utility_score(const JsonValue& spec, const Blackboard& bb);
float utility_curve(const std::string& curve, float x, float threshold);

class BtInstance {
 public:
  BtInstance(std::shared_ptr<const BtTree> tree, std::uint64_t seed);
  BtStatus tick(float dt, BtHost& host);
  void reset(BtHost* host = nullptr);

  Blackboard& blackboard() { return bb_; }
  const Blackboard& blackboard() const { return bb_; }
  const BtTree& tree() const { return *tree_; }
  std::shared_ptr<const BtTree> tree_ptr() const { return tree_; }
  double time() const { return now_; }
  std::uint64_t ticks() const { return ticks_; }
  BtStatus last_status() const { return last_; }

  // Debug: per node, the status it returned and the tick it last ran.
  BtStatus node_status(int i) const { return state_[static_cast<std::size_t>(i)].status; }
  std::uint64_t node_tick(int i) const { return state_[static_cast<std::size_t>(i)].tick; }
  // Labels from the root to the deepest running node ("root > chase").
  std::string running_path() const;
  int running_leaf() const;
  // Record (tick, node, status) for every node run (determinism checks).
  void set_tracing(bool on) { tracing_ = on; }
  struct TraceEntry {
    std::uint64_t tick;
    int node;
    BtStatus status;
    bool operator==(const TraceEntry& o) const = default;
  };
  const std::vector<TraceEntry>& trace() const { return trace_; }
  float random();

 private:
  struct NodeState {
    BtStatus status = BtStatus::Idle;
    std::uint64_t tick = 0;
    int child = 0;          // running child (composites)
    int count = 0;          // repeat / retry / parallel bookkeeping
    double t0 = 0.0;        // wait / timeout start
    double until = -1.0;    // cooldown end; wait length
    std::vector<int> order; // random composite
    double timer = 0.0;     // host scratch
    V3 point;
    int counter = 0;
  };
  BtStatus run(int i, BtHost& host);
  void halt(int i, BtHost& host);
  bool check(const BtNode& n) const;
  BtContext context(int i, bool entering);
  void store(int i, const BtContext& c);

  std::shared_ptr<const BtTree> tree_;
  std::vector<NodeState> state_;
  Blackboard bb_;
  std::uint64_t rng_ = 0;
  std::uint64_t seed_ = 0;
  double now_ = 0.0;
  float dt_ = 0.0f;
  std::uint64_t ticks_ = 0;
  BtStatus last_ = BtStatus::Idle;
  bool tracing_ = false;
  std::vector<TraceEntry> trace_;
};

// Blackboard value from JSON (number / bool / string / [x, y, z]).
BbValue bb_from_json(const JsonValue& v);
JsonValue bb_to_json(const BbValue& v);

}  // namespace ai
}  // namespace ts
