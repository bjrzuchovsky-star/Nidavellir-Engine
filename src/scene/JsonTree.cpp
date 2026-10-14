#include "scene/JsonTree.h"

#include "scene/JsonFlat.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace ts {
namespace tombstone {
namespace json {

namespace {

const Value& shared_null() {
  static const Value v;
  return v;
}

const std::string& empty_string() {
  static const std::string s;
  return s;
}

struct Parser {
  const std::string& t;
  std::size_t i = 0;
  std::string err;
  int depth = 0;

  void ws() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
  }
  bool fail(const char* what) {
    if (err.empty()) err = std::string(what) + " at byte " + std::to_string(i);
    return false;
  }
  bool lit(const char* w) {
    const std::size_t n = std::strlen(w);
    if (t.compare(i, n, w) == 0) {
      i += n;
      return true;
    }
    return false;
  }
  static void put_utf8(std::string& o, unsigned cp) {
    if (cp < 0x80) {
      o.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      o.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      o.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      o.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      o.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      o.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      o.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  bool hex4(unsigned* out) {
    if (i + 4 > t.size()) return false;
    unsigned v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = t[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
      else return false;
    }
    *out = v;
    return true;
  }
  bool str(std::string* out) {
    if (i >= t.size() || t[i] != '"') return fail("expected string");
    ++i;
    while (i < t.size()) {
      const char c = t[i++];
      if (c == '"') return true;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (i >= t.size()) break;
      const char e = t[i++];
      switch (e) {
        case 'n': out->push_back('\n'); break;
        case 't': out->push_back('\t'); break;
        case 'r': out->push_back('\r'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'u': {
          unsigned cp = 0;
          if (!hex4(&cp)) return fail("bad \\u escape");
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' &&
              t[i + 1] == 'u') {
            i += 2;
            unsigned lo = 0;
            if (!hex4(&lo)) return fail("bad \\u escape");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          put_utf8(*out, cp);
          break;
        }
        default: out->push_back(e); break;
      }
    }
    return fail("unterminated string");
  }
  bool value(Value* out) {
    if (++depth > 64) return fail("nested too deep");
    ws();
    bool ok = false;
    if (i >= t.size()) {
      ok = fail("unexpected end");
    } else if (t[i] == '{') {
      ++i;
      *out = Value::object();
      ws();
      if (i < t.size() && t[i] == '}') {
        ++i;
        ok = true;
      } else {
        while (true) {
          ws();
          std::string key;
          if (!str(&key)) break;
          ws();
          if (i >= t.size() || t[i] != ':') {
            fail("expected ':'");
            break;
          }
          ++i;
          Value v;
          if (!value(&v)) break;
          out->set(key, std::move(v));
          ws();
          if (i < t.size() && t[i] == ',') {
            ++i;
            continue;
          }
          if (i < t.size() && t[i] == '}') {
            ++i;
            ok = true;
          } else {
            fail("expected ',' or '}'");
          }
          break;
        }
      }
    } else if (t[i] == '[') {
      ++i;
      *out = Value::array();
      ws();
      if (i < t.size() && t[i] == ']') {
        ++i;
        ok = true;
      } else {
        while (true) {
          Value v;
          if (!value(&v)) break;
          out->push(std::move(v));
          ws();
          if (i < t.size() && t[i] == ',') {
            ++i;
            continue;
          }
          if (i < t.size() && t[i] == ']') {
            ++i;
            ok = true;
          } else {
            fail("expected ',' or ']'");
          }
          break;
        }
      }
    } else if (t[i] == '"') {
      std::string s;
      ok = str(&s);
      if (ok) *out = Value::string(std::move(s));
    } else if (lit("true")) {
      *out = Value::boolean(true);
      ok = true;
    } else if (lit("false")) {
      *out = Value::boolean(false);
      ok = true;
    } else if (lit("null")) {
      *out = Value();
      ok = true;
    } else {
      const char* begin = t.c_str() + i;
      char* end = nullptr;
      const double d = std::strtod(begin, &end);
      if (end == begin) {
        ok = fail("unexpected character");
      } else {
        i += static_cast<std::size_t>(end - begin);
        *out = Value::number(d);
        ok = true;
      }
    }
    --depth;
    return ok;
  }
};

void escape(std::string& o, const std::string& s) {
  o.push_back('"');
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      case '\r': o += "\\r"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          o += buf;
        } else {
          o.push_back(c);
        }
    }
  }
  o.push_back('"');
}

bool is_short_number_array(const Value& v) {
  if (!v.is_array() || v.size() > 16) return false;
  for (const Value& e : v.items()) {
    if (!e.is_number()) return false;
  }
  return true;
}

void emit(std::string& o, const Value& v, int indent) {
  const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
  const std::string pad_in(static_cast<std::size_t>(indent + 1) * 2, ' ');
  switch (v.type()) {
    case Value::Type::Null: o += "null"; break;
    case Value::Type::Bool: o += v.as_bool() ? "true" : "false"; break;
    case Value::Type::Number: {
      const double d = v.as_number();
      if (!std::isfinite(d)) {
        o += "0";
      } else if (d == std::floor(d) && std::fabs(d) < 1e15) {
        o += std::to_string(static_cast<long long>(d));
      } else if (static_cast<double>(static_cast<float>(d)) == d) {
        o += json_flat::format_float(static_cast<float>(d));
      } else {
        o += json_flat::format_number(d);
      }
      break;
    }
    case Value::Type::String: escape(o, v.as_string()); break;
    case Value::Type::Array: {
      if (v.size() == 0) {
        o += "[]";
        break;
      }
      if (is_short_number_array(v)) {
        o += "[";
        for (std::size_t k = 0; k < v.size(); ++k) {
          if (k) o += ", ";
          emit(o, v[k], 0);
        }
        o += "]";
        break;
      }
      o += "[\n";
      for (std::size_t k = 0; k < v.size(); ++k) {
        o += pad_in;
        emit(o, v[k], indent + 1);
        o += k + 1 < v.size() ? ",\n" : "\n";
      }
      o += pad + "]";
      break;
    }
    case Value::Type::Object: {
      if (v.members().empty()) {
        o += "{}";
        break;
      }
      o += "{\n";
      std::size_t k = 0;
      for (const auto& [key, val] : v.members()) {
        o += pad_in;
        escape(o, key);
        o += ": ";
        emit(o, val, indent + 1);
        o += ++k < v.members().size() ? ",\n" : "\n";
      }
      o += pad + "}";
      break;
    }
  }
}

}  // namespace

Value Value::boolean(bool b) {
  Value v;
  v.type_ = Type::Bool;
  v.b_ = b;
  return v;
}
Value Value::number(double d) {
  Value v;
  v.type_ = Type::Number;
  v.d_ = d;
  return v;
}
Value Value::string(std::string s) {
  Value v;
  v.type_ = Type::String;
  v.s_ = std::move(s);
  return v;
}
Value Value::array() {
  Value v;
  v.type_ = Type::Array;
  return v;
}
Value Value::object() {
  Value v;
  v.type_ = Type::Object;
  return v;
}

const std::string& Value::as_string() const {
  return is_string() ? s_ : empty_string();
}
std::size_t Value::size() const {
  return is_array() ? arr_.size() : (is_object() ? obj_.size() : 0);
}
const Value& Value::operator[](std::size_t i) const {
  return (is_array() && i < arr_.size()) ? arr_[i] : shared_null();
}
Value& Value::push(Value v) {
  if (!is_array()) *this = array();
  arr_.push_back(std::move(v));
  return arr_.back();
}
const Value& Value::operator[](const std::string& key) const {
  if (is_object()) {
    for (const auto& [k, v] : obj_) {
      if (k == key) return v;
    }
  }
  return shared_null();
}
bool Value::has(const std::string& key) const {
  if (!is_object()) return false;
  for (const auto& kv : obj_) {
    if (kv.first == key) return true;
  }
  return false;
}
Value& Value::set(const std::string& key, Value v) {
  if (!is_object()) *this = object();
  for (auto& kv : obj_) {
    if (kv.first == key) {
      kv.second = std::move(v);
      return kv.second;
    }
  }
  obj_.emplace_back(key, std::move(v));
  return obj_.back().second;
}
bool Value::floats(const std::string& key, float* out, std::size_t n) const {
  const Value& a = (*this)[key];
  if (!a.is_array() || a.size() != n) return false;
  for (std::size_t k = 0; k < n; ++k) {
    if (!a[k].is_number()) return false;
  }
  for (std::size_t k = 0; k < n; ++k) out[k] = a[k].as_float();
  return true;
}

bool parse(const std::string& text, Value* out, std::string* error) {
  Parser p{text};
  Value v;
  if (!p.value(&v)) {
    if (error) *error = p.err;
    return false;
  }
  p.ws();
  if (p.i != text.size()) {
    if (error) *error = "trailing characters at byte " + std::to_string(p.i);
    return false;
  }
  *out = std::move(v);
  return true;
}

std::string write(const Value& v) {
  std::string o;
  emit(o, v, 0);
  o.push_back('\n');
  return o;
}

Value floats(const float* v, std::size_t n) {
  Value a = Value::array();
  for (std::size_t k = 0; k < n; ++k) a.push(Value::number(v[k]));
  return a;
}

}  // namespace json
}  // namespace tombstone
}  // namespace ts
