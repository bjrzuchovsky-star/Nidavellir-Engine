#pragma once

// Story variables: the small typed store dialogue conditions read, dialogue
// / quest actions write and saves keep ("gold" = 25, "met_hank" = true,
// "posse" = "Rider"). Also the condition / action language both use:
//
//   condition  "met_hank", "!met_hank", "gold >= 10", "name == 'Rider'",
//              "quest:bell == active" (status: inactive / active /
//              completed / failed), "obj:bell/ring >= 1", joined with
//              && and || (no parentheses; && binds tighter).
//   action     "set k v", "add k n", "quest start|complete|fail id",
//              "quest progress q o [n]", "event name [arg]", "save [slot]".

#include "scene/JsonTree.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace story {

struct VarValue {
  enum class Kind { Bool, Number, String } kind = Kind::Bool;
  bool b = false;
  double n = 0.0;
  std::string s;
  static VarValue boolean(bool v) { VarValue x; x.kind = Kind::Bool; x.b = v; return x; }
  static VarValue number(double v) { VarValue x; x.kind = Kind::Number; x.n = v; return x; }
  static VarValue string(std::string v) { VarValue x; x.kind = Kind::String; x.s = std::move(v); return x; }
  // "true" / "false" / a number / 'quoted' or "quoted" / bare word.
  static VarValue parse(const std::string& text);
  bool truthy() const;
  double num() const;
  std::string str() const;
  bool operator==(const VarValue& o) const;
};

class Vars {
 public:
  bool has(const std::string& k) const { return m_.count(k) != 0; }
  VarValue get(const std::string& k) const;  // missing = false
  void set(const std::string& k, VarValue v) { m_[k] = std::move(v); }
  double add(const std::string& k, double n);
  void erase(const std::string& k) { m_.erase(k); }
  void clear() { m_.clear(); }
  const std::map<std::string, VarValue>& all() const { return m_; }
  json::Value to_json() const;
  void from_json(const json::Value& v);
  // "{gold} dollars" -> "25 dollars"; unknown names stay as written.
  std::string format(const std::string& text) const;

 private:
  std::map<std::string, VarValue> m_;
};

// What a condition can ask besides variables (quest status / objective
// progress); null hooks read as inactive / 0.
struct ConditionEnv {
  const Vars* vars = nullptr;
  std::function<std::string(const std::string& quest)> quest_status;
  std::function<int(const std::string& quest, const std::string& objective)> objective;
};
bool eval_condition(const std::string& cond, const ConditionEnv& env, std::string* error = nullptr);
// Syntax only (for lint).
bool check_condition(const std::string& cond, std::string* error);

// One parsed action ("quest progress bell ring 1" -> verb "quest",
// args {"progress","bell","ring","1"}).
struct Action {
  std::string verb;
  std::vector<std::string> args;
};
bool parse_action(const std::string& text, Action* out, std::string* error);

}  // namespace story
}  // namespace tombstone
}  // namespace ts
