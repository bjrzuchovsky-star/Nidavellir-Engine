#pragma once

// A small JSON value for the AI toolkit's data files (*.bt.json, agent
// settings). Strict enough for hand-written files: objects keep key order,
// numbers are doubles, comments are not allowed.

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ts {
namespace ai {

class JsonValue {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  JsonValue() = default;
  static JsonValue number(double v);
  static JsonValue boolean(bool v);
  static JsonValue string(std::string v);
  static JsonValue array();
  static JsonValue object();

  static bool parse(std::string_view text, JsonValue* out, std::string* error);
  std::string dump(int indent = 2) const;

  Type type() const { return type_; }
  bool is_object() const { return type_ == Type::Object; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_string() const { return type_ == Type::String; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_null() const { return type_ == Type::Null; }

  double num(double fallback = 0.0) const { return type_ == Type::Number ? num_ : fallback; }
  bool flag(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
  const std::string& str() const { return str_; }
  std::string str_or(const std::string& fallback) const {
    return type_ == Type::String ? str_ : fallback;
  }

  const std::vector<JsonValue>& items() const { return arr_; }
  const std::vector<std::pair<std::string, JsonValue>>& members() const { return obj_; }
  // Null value when missing.
  const JsonValue& get(const std::string& key) const;
  bool has(const std::string& key) const;
  void set(const std::string& key, JsonValue v);
  void push(JsonValue v) { arr_.push_back(std::move(v)); }

  bool operator==(const JsonValue& o) const = default;

 private:
  Type type_ = Type::Null;
  double num_ = 0.0;
  bool bool_ = false;
  std::string str_;
  std::vector<JsonValue> arr_;
  std::vector<std::pair<std::string, JsonValue>> obj_;
};

}  // namespace ai
}  // namespace ts
