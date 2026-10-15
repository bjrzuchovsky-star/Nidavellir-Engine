#include "ai/BehaviorTree.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace ts {
namespace ai {

// --- Blackboard -----------------------------------------------------------------------

bool BbValue::truthy() const {
  switch (type) {
    case Type::None: return false;
    case Type::Number: return num != 0.0;
    case Type::Bool: return flag;
    case Type::Text: return !text.empty();
    case Type::Vec: return true;
  }
  return false;
}

double BbValue::number() const {
  switch (type) {
    case Type::Number: return num;
    case Type::Bool: return flag ? 1.0 : 0.0;
    case Type::Text: return std::atof(text.c_str());
    case Type::Vec: return length(vec);
    case Type::None: break;
  }
  return 0.0;
}

std::string BbValue::str() const {
  switch (type) {
    case Type::Number: {
      char b[32];
      std::snprintf(b, sizeof(b), "%g", num);
      return b;
    }
    case Type::Bool: return flag ? "true" : "false";
    case Type::Text: return text;
    case Type::Vec: {
      char b[96];
      std::snprintf(b, sizeof(b), "(%.2f, %.2f, %.2f)", vec.x, vec.y, vec.z);
      return b;
    }
    case Type::None: break;
  }
  return "nil";
}

const BbValue* Blackboard::get(const std::string& key) const {
  const auto it = vals_.find(key);
  return it == vals_.end() ? nullptr : &it->second;
}
double Blackboard::number(const std::string& key, double fallback) const {
  const BbValue* v = get(key);
  return v ? v->number() : fallback;
}
bool Blackboard::flag(const std::string& key, bool fallback) const {
  const BbValue* v = get(key);
  return v ? v->truthy() : fallback;
}
std::string Blackboard::text(const std::string& key, const std::string& fallback) const {
  const BbValue* v = get(key);
  return v ? v->str() : fallback;
}
bool Blackboard::vec(const std::string& key, V3* out) const {
  const BbValue* v = get(key);
  if (!v || v->type != BbValue::Type::Vec) return false;
  *out = v->vec;
  return true;
}

BbValue bb_from_json(const JsonValue& v) {
  if (v.is_number()) return BbValue::of_number(v.num());
  if (v.is_bool()) return BbValue::of_bool(v.flag());
  if (v.is_string()) return BbValue::of_text(v.str());
  if (v.is_array() && v.items().size() >= 2) {
    const auto& it = v.items();
    return BbValue::of_vec({static_cast<float>(it[0].num()), static_cast<float>(it[1].num()),
                            it.size() > 2 ? static_cast<float>(it[2].num()) : 0.0f});
  }
  return {};
}

JsonValue bb_to_json(const BbValue& v) {
  switch (v.type) {
    case BbValue::Type::Number: return JsonValue::number(v.num);
    case BbValue::Type::Bool: return JsonValue::boolean(v.flag);
    case BbValue::Type::Text: return JsonValue::string(v.text);
    case BbValue::Type::Vec: {
      JsonValue a = JsonValue::array();
      a.push(JsonValue::number(v.vec.x));
      a.push(JsonValue::number(v.vec.y));
      a.push(JsonValue::number(v.vec.z));
      return a;
    }
    case BbValue::Type::None: break;
  }
  return {};
}

// --- Node catalogue -------------------------------------------------------------------

const char* to_string(BtStatus s) {
  switch (s) {
    case BtStatus::Success: return "success";
    case BtStatus::Failure: return "failure";
    case BtStatus::Running: return "running";
    case BtStatus::Idle: break;
  }
  return "idle";
}

const char* to_string(BtKind k) {
  switch (k) {
    case BtKind::Composite: return "Composite";
    case BtKind::Decorator: return "Decorator";
    case BtKind::Condition: return "Condition";
    case BtKind::Action: break;
  }
  return "Action";
}

const std::vector<BtNodeInfo>& bt_node_types() {
  static const std::vector<BtNodeInfo> kTypes = {
      {"sequence", BtKind::Composite, "reactive", "Runs children in order until one fails"},
      {"selector", BtKind::Composite, "reactive", "Runs children in order until one succeeds"},
      {"parallel", BtKind::Composite, "success failure", "Runs every child each tick"},
      {"random", BtKind::Composite, "mode", "Shuffled selector (mode \"sequence\" for a shuffled sequence)"},
      {"utility", BtKind::Composite, "hysteresis", "Runs the child with the best utility score"},
      {"inverter", BtKind::Decorator, "", "Success <-> failure"},
      {"succeeder", BtKind::Decorator, "", "Always succeeds once the child finishes"},
      {"failer", BtKind::Decorator, "", "Always fails once the child finishes"},
      {"repeat", BtKind::Decorator, "times", "Repeats the child (0 = forever) while it succeeds"},
      {"retry", BtKind::Decorator, "times", "Retries the child after failures"},
      {"cooldown", BtKind::Decorator, "seconds", "Fails until `seconds` after the child last finished"},
      {"timeout", BtKind::Decorator, "seconds", "Fails (and halts the child) after `seconds`"},
      {"if", BtKind::Decorator, "key op value", "Runs the child while the blackboard check holds"},
      {"check", BtKind::Condition, "key op value", "Blackboard compare: == != < <= > >= set unset true false"},
      {"chance", BtKind::Condition, "p", "Succeeds with probability p (seeded)"},
      {"can_see", BtKind::Condition, "target key", "A target is in the sight cone with a clear line", true},
      {"heard", BtKind::Condition, "within key", "A sound was heard in the last `within` seconds", true},
      {"near", BtKind::Condition, "key distance", "Within `distance` of the blackboard position / target", true},
      {"wait", BtKind::Action, "seconds max", "Runs for `seconds` (random up to `max`)"},
      {"set", BtKind::Action, "key value", "Writes the blackboard (\"$other\" copies a key)"},
      {"inc", BtKind::Action, "key by", "Adds to a blackboard number"},
      {"log", BtKind::Action, "text", "Telegraph line"},
      {"succeed", BtKind::Action, "", "Succeeds"},
      {"fail", BtKind::Action, "", "Fails"},
      {"running", BtKind::Action, "", "Runs forever"},
      {"move_to", BtKind::Action, "key speed", "Paths to the blackboard position", true},
      {"wander", BtKind::Action, "radius speed", "Walks to a random reachable point near home", true},
      {"chase", BtKind::Action, "key speed range", "Follows the target until within range", true},
      {"flee", BtKind::Action, "key distance", "Runs away from the target", true},
      {"attack", BtKind::Action, "key damage", "Strikes the target when in reach", true},
      {"face", BtKind::Action, "key", "Turns toward the target", true},
      {"play_anim", BtKind::Action, "clip fade", "Plays an animation state / clip", true},
      {"stop", BtKind::Action, "", "Stops moving", true},
      {"lua", BtKind::Action, "fn", "Calls fn(blackboard) on the entity's Lua script", true},
  };
  return kTypes;
}

const BtNodeInfo* bt_node_info(const std::string& type) {
  for (const BtNodeInfo& i : bt_node_types()) {
    if (type == i.type) return &i;
  }
  return nullptr;
}

// --- Tree load / save -------------------------------------------------------------------

namespace {

bool read_node(const JsonValue& j, int parent, BtTree* t, std::string* error, int depth) {
  if (depth > 64) {
    *error = "tree nested too deep";
    return false;
  }
  if (!j.is_object()) {
    *error = "a node must be an object";
    return false;
  }
  BtNode n;
  n.type = j.get("type").str();
  if (n.type.empty()) {
    *error = "a node has no \"type\"";
    return false;
  }
  n.name = j.get("name").str();
  n.parent = parent;
  n.params = JsonValue::object();
  for (const auto& [k, v] : j.members()) {
    if (k != "type" && k != "name" && k != "children" && k != "child") n.params.set(k, v);
  }
  const int me = static_cast<int>(t->nodes.size());
  t->nodes.push_back(std::move(n));
  if (parent >= 0) t->nodes[static_cast<std::size_t>(parent)].children.push_back(me);
  if (j.has("child")) {
    if (!read_node(j.get("child"), me, t, error, depth + 1)) return false;
  }
  for (const JsonValue& c : j.get("children").items()) {
    if (!read_node(c, me, t, error, depth + 1)) return false;
  }
  if (t->nodes.size() > 4096) {
    *error = "tree has too many nodes (4096 max)";
    return false;
  }
  return true;
}

JsonValue write_node(const BtTree& t, int i) {
  const BtNode& n = t.nodes[static_cast<std::size_t>(i)];
  JsonValue j = JsonValue::object();
  j.set("type", JsonValue::string(n.type));
  if (!n.name.empty()) j.set("name", JsonValue::string(n.name));
  for (const auto& [k, v] : n.params.members()) j.set(k, v);
  const BtNodeInfo* info = bt_node_info(n.type);
  if (info && info->kind == BtKind::Decorator && n.children.size() == 1) {
    j.set("child", write_node(t, n.children[0]));
  } else if (!n.children.empty()) {
    JsonValue arr = JsonValue::array();
    for (int c : n.children) arr.push(write_node(t, c));
    j.set("children", std::move(arr));
  }
  return j;
}

}  // namespace

bool BtTree::from_json(const JsonValue& j, BtTree* out, std::string* error) {
  std::string scratch;
  std::string& err = error ? *error : scratch;
  BtTree t;
  if (!j.is_object() || !j.get("root").is_object()) {
    err = "expected {\"root\": {...}}";
    return false;
  }
  t.name = j.get("name").str();
  t.blackboard = j.get("blackboard").is_object() ? j.get("blackboard") : JsonValue::object();
  if (!read_node(j.get("root"), -1, &t, &err, 0)) return false;
  *out = std::move(t);
  return true;
}

bool BtTree::parse(const std::string& text, std::string* error) {
  JsonValue j;
  if (!JsonValue::parse(text, &j, error)) return false;
  return from_json(j, this, error);
}

JsonValue BtTree::to_json() const {
  JsonValue j = JsonValue::object();
  if (!name.empty()) j.set("name", JsonValue::string(name));
  if (blackboard.is_object() && !blackboard.members().empty()) j.set("blackboard", blackboard);
  if (!nodes.empty()) j.set("root", write_node(*this, 0));
  return j;
}

std::vector<std::string> BtTree::lint() const {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const BtNode& n = nodes[i];
    const BtNodeInfo* info = bt_node_info(n.type);
    if (!info) {
      out.push_back("\"" + n.label() + "\": unknown type \"" + n.type + "\" (the host may still answer it)");
      continue;
    }
    if (info->kind == BtKind::Decorator && n.children.size() != 1) {
      out.push_back("\"" + n.label() + "\": a " + n.type + " needs exactly one child");
    }
    if (info->kind == BtKind::Composite && n.children.empty()) {
      out.push_back("\"" + n.label() + "\": a " + n.type + " with no children");
    }
    if ((info->kind == BtKind::Condition || info->kind == BtKind::Action) && !n.children.empty()) {
      out.push_back("\"" + n.label() + "\": a " + n.type + " ignores its children");
    }
  }
  return out;
}

int BtTree::add(int parent, const std::string& type) {
  BtNode n;
  n.type = type;
  n.params = JsonValue::object();
  if (parent < 0 || nodes.empty()) {
    // New root: the old root (if any) becomes its child.
    nodes.insert(nodes.begin(), std::move(n));
    for (BtNode& m : nodes) {
      for (int& c : m.children) ++c;
      if (m.parent >= 0) ++m.parent;
    }
    if (nodes.size() > 1) {
      nodes[1].parent = 0;
      nodes[0].children.push_back(1);
    }
    nodes[0].parent = -1;
    return 0;
  }
  n.parent = parent;
  nodes.push_back(std::move(n));
  const int me = static_cast<int>(nodes.size()) - 1;
  nodes[static_cast<std::size_t>(parent)].children.push_back(me);
  return me;
}

void BtTree::remove(int node) {
  if (node < 0 || node >= static_cast<int>(nodes.size())) return;
  if (node == 0) {
    nodes.clear();
    return;
  }
  // Rebuild without the subtree (keeps indices compact and parents first).
  const JsonValue j = to_json();
  std::vector<bool> drop(nodes.size(), false);
  std::vector<int> stack{node};
  while (!stack.empty()) {
    const int k = stack.back();
    stack.pop_back();
    drop[static_cast<std::size_t>(k)] = true;
    for (int c : nodes[static_cast<std::size_t>(k)].children) stack.push_back(c);
  }
  BtTree t;
  t.name = name;
  t.blackboard = blackboard;
  std::vector<int> remap(nodes.size(), -1);
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (drop[i]) continue;
    remap[i] = static_cast<int>(t.nodes.size());
    t.nodes.push_back(nodes[i]);
  }
  for (BtNode& n : t.nodes) {
    n.parent = n.parent >= 0 ? remap[static_cast<std::size_t>(n.parent)] : -1;
    std::vector<int> kids;
    for (int c : n.children) {
      if (remap[static_cast<std::size_t>(c)] >= 0) kids.push_back(remap[static_cast<std::size_t>(c)]);
    }
    n.children = kids;
  }
  (void)j;
  *this = std::move(t);
}

void BtTree::move(int node, int delta) {
  if (node <= 0 || node >= static_cast<int>(nodes.size())) return;
  auto& kids = nodes[static_cast<std::size_t>(nodes[static_cast<std::size_t>(node)].parent)].children;
  const auto it = std::find(kids.begin(), kids.end(), node);
  const long at = it - kids.begin();
  const long to = at + delta;
  if (to < 0 || to >= static_cast<long>(kids.size())) return;
  std::swap(kids[static_cast<std::size_t>(at)], kids[static_cast<std::size_t>(to)]);
}

// --- Utility ----------------------------------------------------------------------------

float utility_curve(const std::string& curve, float x, float threshold) {
  x = std::clamp(x, 0.0f, 1.0f);
  if (curve == "quadratic") return x * x;
  if (curve == "sqrt") return std::sqrt(x);
  if (curve == "inverse") return 1.0f - x;
  if (curve == "logistic") return 1.0f / (1.0f + std::exp(-10.0f * (x - 0.5f)));
  if (curve == "step") return x >= threshold ? 1.0f : 0.0f;
  return x;  // linear
}

float utility_score(const JsonValue& spec, const Blackboard& bb) {
  if (!spec.is_object()) return 0.0f;
  float score = static_cast<float>(spec.get("weight").num(1.0));
  for (const JsonValue& c : spec.get("considerations").items()) {
    const float lo = static_cast<float>(c.get("min").num(0.0));
    const float hi = static_cast<float>(c.get("max").num(1.0));
    const float raw = static_cast<float>(bb.number(c.get("key").str(), c.get("default").num(0.0)));
    const float x = hi > lo ? (raw - lo) / (hi - lo) : raw;
    score *= utility_curve(c.get("curve").str_or("linear"), x, static_cast<float>(c.get("threshold").num(0.5)));
  }
  return score + static_cast<float>(spec.get("bonus").num(0.0));
}

// --- Runtime ----------------------------------------------------------------------------

float BtContext::random() { return bt ? bt->random() : 0.5f; }

BtInstance::BtInstance(std::shared_ptr<const BtTree> tree, std::uint64_t seed)
    : tree_(std::move(tree)), seed_(seed) {
  reset();
}

float BtInstance::random() {
  // splitmix64
  std::uint64_t z = (rng_ += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  z ^= z >> 31;
  return static_cast<float>(z >> 40) / static_cast<float>(1ull << 24);
}

void BtInstance::reset(BtHost* host) {
  if (host && tree_ && !tree_->nodes.empty()) halt(0, *host);
  state_.assign(tree_ ? tree_->nodes.size() : 0, NodeState{});
  bb_.clear();
  if (tree_) {
    for (const auto& [k, v] : tree_->blackboard.members()) bb_.set(k, bb_from_json(v));
  }
  rng_ = seed_ ^ 0x5eed5eed5eedull;
  now_ = 0.0;
  ticks_ = 0;
  last_ = BtStatus::Idle;
  trace_.clear();
}

BtStatus BtInstance::tick(float dt, BtHost& host) {
  if (!tree_ || tree_->nodes.empty()) return BtStatus::Failure;
  dt_ = dt;
  ++ticks_;
  last_ = run(0, host);
  if (last_ != BtStatus::Running) {
    // Root finished: next tick starts over (fresh node states, same board).
    for (NodeState& s : state_) {
      const BtStatus keep = s.status;
      const std::uint64_t t = s.tick;
      const double until = s.until;
      s = NodeState{};
      s.status = keep;
      s.tick = t;
      s.until = until;  // cooldowns survive restarts
    }
  }
  now_ += dt;
  return last_;
}

BtContext BtInstance::context(int i, bool entering) {
  BtContext c;
  c.bt = this;
  c.bb = &bb_;
  c.node = &tree_->nodes[static_cast<std::size_t>(i)];
  c.index = i;
  c.now = now_;
  c.dt = dt_;
  c.entering = entering;
  const NodeState& s = state_[static_cast<std::size_t>(i)];
  c.timer = s.timer;
  c.point = s.point;
  c.counter = s.counter;
  return c;
}

void BtInstance::store(int i, const BtContext& c) {
  NodeState& s = state_[static_cast<std::size_t>(i)];
  s.timer = c.timer;
  s.point = c.point;
  s.counter = c.counter;
}

void BtInstance::halt(int i, BtHost& host) {
  NodeState& s = state_[static_cast<std::size_t>(i)];
  if (s.status != BtStatus::Running) return;
  const BtNode& n = tree_->nodes[static_cast<std::size_t>(i)];
  for (int c : n.children) halt(c, host);
  const BtNodeInfo* info = bt_node_info(n.type);
  if (!info || info->host) {
    BtContext ctx = context(i, false);
    host.halt(ctx);
  }
  const double until = s.until;
  s = NodeState{};
  s.status = BtStatus::Idle;
  if (n.type == "cooldown") s.until = until;
}

bool BtInstance::check(const BtNode& n) const {
  const std::string key = n.text("key");
  const std::string op = n.text("op", "true");
  const BbValue* v = bb_.get(key);
  if (op == "set") return v != nullptr;
  if (op == "unset") return v == nullptr;
  if (op == "true") return v && v->truthy();
  if (op == "false") return !v || !v->truthy();
  const JsonValue& rhs = n.param("value");
  if (rhs.is_string()) {
    const std::string have = v ? v->str() : "";
    std::string want = rhs.str();
    if (!want.empty() && want[0] == '$') want = bb_.text(want.substr(1));
    if (op == "==") return have == want;
    if (op == "!=") return have != want;
    return false;
  }
  const double a = v ? v->number() : 0.0;
  const double b = rhs.num(rhs.flag() ? 1.0 : 0.0);
  if (op == "==") return a == b;
  if (op == "!=") return a != b;
  if (op == "<") return a < b;
  if (op == "<=") return a <= b;
  if (op == ">") return a > b;
  if (op == ">=") return a >= b;
  return false;
}

BtStatus BtInstance::run(int i, BtHost& host) {
  const BtNode& n = tree_->nodes[static_cast<std::size_t>(i)];
  NodeState& s = state_[static_cast<std::size_t>(i)];
  const bool entering = s.status != BtStatus::Running;
  if (entering) {
    s.child = 0;
    s.count = 0;
    s.t0 = now_;
    s.timer = 0.0;
    s.counter = 0;
    s.point = V3{};
    s.order.clear();
  }
  const std::string& t = n.type;
  const auto& kids = n.children;
  BtStatus r = BtStatus::Failure;

  auto finish = [&](BtStatus st) {
    NodeState& ss = state_[static_cast<std::size_t>(i)];
    ss.status = st;
    ss.tick = ticks_;
    if (tracing_) trace_.push_back({ticks_, i, st});
    return st;
  };

  if (t == "sequence" || t == "selector") {
    const bool seq = t == "sequence";
    const bool reactive = n.param("reactive").flag(false);
    const int start = reactive ? 0 : s.child;
    r = seq ? BtStatus::Success : BtStatus::Failure;
    for (int k = start; k < static_cast<int>(kids.size()); ++k) {
      const BtStatus c = run(kids[static_cast<std::size_t>(k)], host);
      if (c == BtStatus::Running) {
        // A reactive branch took over: halt whatever ran later before.
        for (int m = k + 1; m < static_cast<int>(kids.size()); ++m) halt(kids[static_cast<std::size_t>(m)], host);
        state_[static_cast<std::size_t>(i)].child = k;
        return finish(BtStatus::Running);
      }
      if (c == (seq ? BtStatus::Failure : BtStatus::Success)) {
        for (int m = k + 1; m < static_cast<int>(kids.size()); ++m) halt(kids[static_cast<std::size_t>(m)], host);
        r = c;
        break;
      }
    }
    return finish(r);
  }
  if (t == "random") {
    if (entering || s.order.size() != kids.size()) {
      std::vector<int> order(kids.size());
      for (std::size_t k = 0; k < kids.size(); ++k) order[k] = static_cast<int>(k);
      for (std::size_t k = order.size(); k > 1; --k) {
        const std::size_t j = static_cast<std::size_t>(random() * static_cast<float>(k)) % k;
        std::swap(order[k - 1], order[j]);
      }
      state_[static_cast<std::size_t>(i)].order = order;
    }
    const bool seq = n.text("mode") == "sequence";
    r = seq ? BtStatus::Success : BtStatus::Failure;
    const std::vector<int> order = state_[static_cast<std::size_t>(i)].order;
    for (int k = state_[static_cast<std::size_t>(i)].child; k < static_cast<int>(order.size()); ++k) {
      const BtStatus c = run(kids[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])], host);
      if (c == BtStatus::Running) {
        state_[static_cast<std::size_t>(i)].child = k;
        return finish(BtStatus::Running);
      }
      if (c == (seq ? BtStatus::Failure : BtStatus::Success)) {
        r = c;
        break;
      }
    }
    return finish(r);
  }
  if (t == "parallel") {
    const int need_s = static_cast<int>(n.num("success", static_cast<double>(kids.size())));
    const int need_f = static_cast<int>(n.num("failure", 1.0));
    if (entering || s.order.size() != kids.size()) state_[static_cast<std::size_t>(i)].order.assign(kids.size(), 0);
    int ok = 0, bad = 0;
    for (std::size_t k = 0; k < kids.size(); ++k) {
      int done = state_[static_cast<std::size_t>(i)].order[k];  // 0 pending, 1 success, 2 failure
      if (done == 0) {
        const BtStatus cs = run(kids[k], host);
        done = cs == BtStatus::Success ? 1 : cs == BtStatus::Failure ? 2 : 0;
        state_[static_cast<std::size_t>(i)].order[k] = done;
      }
      ok += done == 1 ? 1 : 0;
      bad += done == 2 ? 1 : 0;
    }
    if (ok >= need_s || bad >= need_f) {
      for (int c : kids) halt(c, host);
      return finish(ok >= need_s ? BtStatus::Success : BtStatus::Failure);
    }
    return finish(BtStatus::Running);
  }
  if (t == "utility") {
    float best = 0.0f;
    int pick = -1;
    std::vector<float> scores(kids.size(), 0.0f);
    for (std::size_t k = 0; k < kids.size(); ++k) {
      scores[k] = utility_score(tree_->nodes[static_cast<std::size_t>(kids[k])].param("utility"), bb_);
      if (scores[k] > best) {
        best = scores[k];
        pick = static_cast<int>(k);
      }
    }
    const int cur = entering ? -1 : s.child;
    const float hyst = static_cast<float>(n.num("hysteresis", 0.1));
    if (cur >= 0 && cur < static_cast<int>(kids.size()) &&
        state_[static_cast<std::size_t>(kids[static_cast<std::size_t>(cur)])].status == BtStatus::Running &&
        scores[static_cast<std::size_t>(cur)] > 0.0f && scores[static_cast<std::size_t>(cur)] + hyst >= best) {
      pick = cur;
    }
    if (pick < 0) return finish(BtStatus::Failure);
    if (cur >= 0 && cur != pick) halt(kids[static_cast<std::size_t>(cur)], host);
    state_[static_cast<std::size_t>(i)].child = pick;
    bb_.set_text("utility_choice", tree_->nodes[static_cast<std::size_t>(kids[static_cast<std::size_t>(pick)])].label());
    return finish(run(kids[static_cast<std::size_t>(pick)], host));
  }
  // Decorators.
  const BtNodeInfo* info = bt_node_info(t);
  if (info && info->kind == BtKind::Decorator) {
    if (kids.empty()) return finish(BtStatus::Failure);
    const int c = kids[0];
    if (t == "cooldown") {
      if (entering && s.until > now_) return finish(BtStatus::Failure);
      const BtStatus cs = run(c, host);
      if (cs != BtStatus::Running) state_[static_cast<std::size_t>(i)].until = now_ + n.num("seconds", 1.0);
      return finish(cs);
    }
    if (t == "timeout") {
      if (now_ - s.t0 > n.num("seconds", 1.0)) {
        halt(c, host);
        return finish(BtStatus::Failure);
      }
      return finish(run(c, host));
    }
    if (t == "if") {
      if (!check(n)) {
        halt(c, host);
        return finish(BtStatus::Failure);
      }
      return finish(run(c, host));
    }
    const BtStatus cs = run(c, host);
    if (cs == BtStatus::Running) return finish(BtStatus::Running);
    if (t == "inverter") return finish(cs == BtStatus::Success ? BtStatus::Failure : BtStatus::Success);
    if (t == "succeeder") return finish(BtStatus::Success);
    if (t == "failer") return finish(BtStatus::Failure);
    if (t == "repeat" || t == "retry") {
      const bool rep = t == "repeat";
      if (cs == (rep ? BtStatus::Failure : BtStatus::Success)) return finish(cs);
      const int times = static_cast<int>(n.num("times", rep ? 0.0 : 3.0));
      NodeState& ss = state_[static_cast<std::size_t>(i)];
      ++ss.count;
      if (times > 0 && ss.count >= times) return finish(cs);
      // Go again next tick (no infinite loop inside one tick).
      state_[static_cast<std::size_t>(c)].status = BtStatus::Idle;
      return finish(BtStatus::Running);
    }
    return finish(cs);
  }
  // Leaves.
  if (t == "check") return finish(check(n) ? BtStatus::Success : BtStatus::Failure);
  if (t == "chance") return finish(random() < n.num("p", 0.5) ? BtStatus::Success : BtStatus::Failure);
  if (t == "succeed") return finish(BtStatus::Success);
  if (t == "fail") return finish(BtStatus::Failure);
  if (t == "running") return finish(BtStatus::Running);
  if (t == "wait") {
    if (entering) {
      const double lo = n.num("seconds", 1.0);
      const double hi = n.num("max", lo);
      state_[static_cast<std::size_t>(i)].until = lo + (hi > lo ? (hi - lo) * random() : 0.0);
    }
    const NodeState& ss = state_[static_cast<std::size_t>(i)];
    return finish(now_ + dt_ - ss.t0 >= ss.until - 1e-9 ? BtStatus::Success : BtStatus::Running);
  }
  if (t == "set") {
    const JsonValue& v = n.param("value");
    if (v.is_string() && !v.str().empty() && v.str()[0] == '$') {
      const BbValue* src = bb_.get(v.str().substr(1));
      if (src) bb_.set(n.text("key"), *src);
      else bb_.erase(n.text("key"));
    } else if (v.is_null()) {
      bb_.erase(n.text("key"));
    } else {
      bb_.set(n.text("key"), bb_from_json(v));
    }
    return finish(BtStatus::Success);
  }
  if (t == "inc") {
    const std::string key = n.text("key");
    bb_.set_number(key, bb_.number(key) + n.num("by", 1.0));
    return finish(BtStatus::Success);
  }
  if (t == "log") {
    BtContext ctx = context(i, entering);
    host.log(ctx, n.text("text"));
    return finish(BtStatus::Success);
  }
  // Host conditions / actions.
  BtContext ctx = context(i, entering);
  if (info && info->kind == BtKind::Condition) {
    const bool ok = host.condition(ctx);
    store(i, ctx);
    return finish(ok ? BtStatus::Success : BtStatus::Failure);
  }
  r = host.action(ctx);
  store(i, ctx);
  return finish(r);
}

std::string BtInstance::running_path() const {
  if (!tree_ || tree_->nodes.empty()) return "";
  std::string out;
  int i = 0;
  for (int guard = 0; guard < 64 && i >= 0; ++guard) {
    const BtNode& n = tree_->nodes[static_cast<std::size_t>(i)];
    if (state_[static_cast<std::size_t>(i)].status != BtStatus::Running) break;
    if (!out.empty()) out += " > ";
    out += n.label();
    int next = -1;
    for (int c : n.children) {
      if (state_[static_cast<std::size_t>(c)].status == BtStatus::Running &&
          state_[static_cast<std::size_t>(c)].tick == state_[static_cast<std::size_t>(i)].tick) {
        next = c;
        break;
      }
    }
    i = next;
  }
  return out;
}

int BtInstance::running_leaf() const {
  if (!tree_ || tree_->nodes.empty()) return -1;
  int i = 0, leaf = -1;
  for (int guard = 0; guard < 64 && i >= 0; ++guard) {
    if (state_[static_cast<std::size_t>(i)].status != BtStatus::Running) break;
    leaf = i;
    int next = -1;
    for (int c : tree_->nodes[static_cast<std::size_t>(i)].children) {
      if (state_[static_cast<std::size_t>(c)].status == BtStatus::Running &&
          state_[static_cast<std::size_t>(c)].tick == state_[static_cast<std::size_t>(i)].tick) {
        next = c;
        break;
      }
    }
    i = next;
  }
  return leaf;
}

}  // namespace ai
}  // namespace ts
