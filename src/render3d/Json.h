#pragma once

// Small tree JSON reader / writer for the 3D scene format (scene.json v7)
// and glTF-adjacent data. Objects keep key order so written files diff
// cleanly. Numbers are doubles; floats are written shortest-round-trip.

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  static Value null() { return Value(); }
  static Value boolean(bool b);
  static Value number(double d);
  static Value string(std::string s);
  static Value array();
  static Value object();

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool(bool fallback = false) const { return is_bool() ? b_ : fallback; }
  double as_number(double fallback = 0.0) const { return is_number() ? d_ : fallback; }
  float as_float(float fallback = 0.0f) const {
    return is_number() ? static_cast<float>(d_) : fallback;
  }
  const std::string& as_string() const;
  std::string as_string_or(const std::string& fallback) const {
    return is_string() ? s_ : fallback;
  }

  // Arrays.
  std::size_t size() const;
  const Value& operator[](std::size_t i) const;
  Value& push(Value v);
  const std::vector<Value>& items() const { return arr_; }

  // Objects (missing key -> a shared null).
  const Value& operator[](const std::string& key) const;
  bool has(const std::string& key) const;
  Value& set(const std::string& key, Value v);
  const std::vector<std::pair<std::string, Value>>& members() const { return obj_; }

  // Convenience readers.
  double num(const std::string& key, double fallback) const {
    return (*this)[key].as_number(fallback);
  }
  float flt(const std::string& key, float fallback) const {
    return (*this)[key].as_float(fallback);
  }
  bool flag(const std::string& key, bool fallback) const {
    return (*this)[key].as_bool(fallback);
  }
  std::string str(const std::string& key, const std::string& fallback = {}) const {
    return (*this)[key].as_string_or(fallback);
  }
  // Fill out[0..n) from a number array; false (out untouched) otherwise.
  bool floats(const std::string& key, float* out, std::size_t n) const;

 private:
  Type type_ = Type::Null;
  bool b_ = false;
  double d_ = 0.0;
  std::string s_;
  std::vector<Value> arr_;
  std::vector<std::pair<std::string, Value>> obj_;
};

// Whole-document parse. Error text says where it stopped.
bool parse(const std::string& text, Value* out, std::string* error);
// Pretty-printed (2-space indent; short number arrays on one line).
std::string write(const Value& v);

Value floats(const float* v, std::size_t n);

}  // namespace json
}  // namespace tombstone
}  // namespace ts
