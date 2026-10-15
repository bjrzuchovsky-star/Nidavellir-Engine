#pragma once

// Per-agent memory shared by behaviour-tree nodes, utility scorers and
// scripts: numbers, flags, text and vectors under string keys.

#include "ai/AiMath.h"

#include <map>
#include <string>

namespace ts {
namespace ai {

struct BbValue {
  enum class Type { None, Number, Bool, Text, Vec };
  Type type = Type::None;
  double num = 0.0;
  bool flag = false;
  std::string text;
  V3 vec;

  static BbValue of_number(double v) {
    BbValue b;
    b.type = Type::Number;
    b.num = v;
    return b;
  }
  static BbValue of_bool(bool v) {
    BbValue b;
    b.type = Type::Bool;
    b.flag = v;
    return b;
  }
  static BbValue of_text(std::string v) {
    BbValue b;
    b.type = Type::Text;
    b.text = std::move(v);
    return b;
  }
  static BbValue of_vec(const V3& v) {
    BbValue b;
    b.type = Type::Vec;
    b.vec = v;
    return b;
  }
  bool truthy() const;
  double number() const;  // bool -> 0/1, text -> parsed, vec -> length
  std::string str() const;
  bool operator==(const BbValue& o) const = default;
};

class Blackboard {
 public:
  void set(const std::string& key, BbValue v) { vals_[key] = std::move(v); }
  void set_number(const std::string& key, double v) { set(key, BbValue::of_number(v)); }
  void set_bool(const std::string& key, bool v) { set(key, BbValue::of_bool(v)); }
  void set_text(const std::string& key, std::string v) { set(key, BbValue::of_text(std::move(v))); }
  void set_vec(const std::string& key, const V3& v) { set(key, BbValue::of_vec(v)); }
  bool has(const std::string& key) const { return vals_.count(key) != 0; }
  void erase(const std::string& key) { vals_.erase(key); }
  const BbValue* get(const std::string& key) const;
  double number(const std::string& key, double fallback = 0.0) const;
  bool flag(const std::string& key, bool fallback = false) const;
  std::string text(const std::string& key, const std::string& fallback = "") const;
  bool vec(const std::string& key, V3* out) const;
  const std::map<std::string, BbValue>& values() const { return vals_; }
  void clear() { vals_.clear(); }
  bool operator==(const Blackboard& o) const = default;

 private:
  std::map<std::string, BbValue> vals_;
};

}  // namespace ai
}  // namespace ts
