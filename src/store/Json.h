#pragma once

// A small JSON value (null / bool / number / string / array / object) with a
// strict parser and a pretty writer. Used by the data-driven parts of the
// editor that need nesting (template.json, the Trading Post catalog and
// remote indexes); flat config keeps using core/JsonMini.h.

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ts {
namespace tombstone {
namespace store {

class Json {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  Json(bool b) : type_(Type::Bool), bool_(b) {}
  Json(double n) : type_(Type::Number), num_(n) {}
  Json(int n) : type_(Type::Number), num_(n) {}
  Json(long long n) : type_(Type::Number), num_(static_cast<double>(n)) {}
  Json(const char* s) : type_(Type::String), str_(s) {}
  Json(std::string s) : type_(Type::String), str_(std::move(s)) {}

  static Json array() { Json j; j.type_ = Type::Array; return j; }
  static Json object() { Json j; j.type_ = Type::Object; return j; }

  // Strict RFC 8259 (no comments / trailing commas). False + reason on junk.
  static bool parse(std::string_view text, Json* out, std::string* error = nullptr);
  // Pretty, two-space indent, object keys in insertion order.
  std::string dump(int indent = 2) const;

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool(bool fallback = false) const { return is_bool() ? bool_ : fallback; }
  double as_number(double fallback = 0.0) const { return is_number() ? num_ : fallback; }
  const std::string& as_string() const;  // "" unless a string
  std::string str_or(const std::string& fallback) const {
    return is_string() ? str_ : fallback;
  }

  // Arrays.
  std::size_t size() const;
  const Json& at(std::size_t i) const;  // null when out of range
  void push(Json v);
  const std::vector<Json>& items() const { return arr_; }

  // Objects (lookups on anything else return null).
  bool has(const std::string& key) const;
  const Json& get(const std::string& key) const;
  Json& set(const std::string& key, Json v);  // replaces, keeps order
  const std::vector<std::pair<std::string, Json>>& members() const { return obj_; }

  // Convenience for typed lookups on objects.
  std::string string_at(const std::string& key, const std::string& fallback = {}) const {
    return get(key).str_or(fallback);
  }
  std::vector<std::string> strings_at(const std::string& key) const;

 private:
  void dump_into(std::string& out, int indent, int depth) const;

  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0.0;
  std::string str_;
  std::vector<Json> arr_;
  std::vector<std::pair<std::string, Json>> obj_;
};

// Whole-file helpers.
bool read_text_file(const std::string& path, std::string* out, std::string* error = nullptr);
bool write_text_file(const std::string& path, const std::string& text,
                     std::string* error = nullptr);
bool read_json_file(const std::string& path, Json* out, std::string* error = nullptr);

}  // namespace store
}  // namespace tombstone
}  // namespace ts
