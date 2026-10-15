#include "ai/AiJson.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ts {
namespace ai {

namespace {

struct Parser {
  std::string_view s;
  std::size_t i = 0;
  std::string err;
  int depth = 0;

  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  bool fail(const char* what) {
    if (err.empty()) {
      int line = 1;
      for (std::size_t k = 0; k < i && k < s.size(); ++k) line += s[k] == '\n' ? 1 : 0;
      err = std::string(what) + " (line " + std::to_string(line) + ")";
    }
    return false;
  }
  bool lit(const char* w) {
    const std::size_t n = std::char_traits<char>::length(w);
    if (s.substr(i, n) != w) return false;
    i += n;
    return true;
  }
  bool string(std::string* out) {
    if (i >= s.size() || s[i] != '"') return fail("expected a string");
    ++i;
    out->clear();
    while (i < s.size()) {
      const char c = s[i++];
      if (c == '"') return true;
      if (c == '\\') {
        if (i >= s.size()) break;
        const char e = s[i++];
        switch (e) {
          case 'n': out->push_back('\n'); break;
          case 't': out->push_back('\t'); break;
          case 'r': out->push_back('\r'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'u': {
            if (i + 4 > s.size()) return fail("bad \\u escape");
            const unsigned cp = static_cast<unsigned>(std::strtoul(std::string(s.substr(i, 4)).c_str(), nullptr, 16));
            i += 4;
            if (cp < 0x80) {
              out->push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
              out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
              out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
              out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
              out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            break;
          }
          default: out->push_back(e);
        }
      } else {
        out->push_back(c);
      }
    }
    return fail("unterminated string");
  }
  bool value(JsonValue* out) {
    if (++depth > 64) return fail("nested too deep");
    ws();
    if (i >= s.size()) return fail("unexpected end");
    const char c = s[i];
    bool ok = true;
    if (c == '{') {
      ++i;
      *out = JsonValue::object();
      ws();
      if (i < s.size() && s[i] == '}') {
        ++i;
      } else {
        for (;;) {
          ws();
          std::string key;
          if (!string(&key)) return false;
          ws();
          if (i >= s.size() || s[i] != ':') return fail("expected ':'");
          ++i;
          JsonValue v;
          if (!value(&v)) return false;
          out->set(key, std::move(v));
          ws();
          if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
          }
          if (i < s.size() && s[i] == '}') {
            ++i;
            break;
          }
          return fail("expected ',' or '}'");
        }
      }
    } else if (c == '[') {
      ++i;
      *out = JsonValue::array();
      ws();
      if (i < s.size() && s[i] == ']') {
        ++i;
      } else {
        for (;;) {
          JsonValue v;
          if (!value(&v)) return false;
          out->push(std::move(v));
          ws();
          if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
          }
          if (i < s.size() && s[i] == ']') {
            ++i;
            break;
          }
          return fail("expected ',' or ']'");
        }
      }
    } else if (c == '"') {
      std::string str;
      ok = string(&str);
      *out = JsonValue::string(std::move(str));
    } else if (lit("true")) {
      *out = JsonValue::boolean(true);
    } else if (lit("false")) {
      *out = JsonValue::boolean(false);
    } else if (lit("null")) {
      *out = JsonValue();
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      const std::string rest(s.substr(i, 40));
      char* end = nullptr;
      const double v = std::strtod(rest.c_str(), &end);
      if (end == rest.c_str()) return fail("bad number");
      i += static_cast<std::size_t>(end - rest.c_str());
      *out = JsonValue::number(v);
    } else {
      return fail("unexpected character");
    }
    --depth;
    return ok;
  }
};

void dump_string(const std::string& s, std::string* out) {
  out->push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\n': *out += "\\n"; break;
      case '\t': *out += "\\t"; break;
      case '\r': *out += "\\r"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", c);
          *out += b;
        } else {
          out->push_back(c);
        }
    }
  }
  out->push_back('"');
}

void dump_value(const JsonValue& v, int indent, int level, std::string* out) {
  const std::string pad = indent > 0 ? std::string(static_cast<std::size_t>(indent * (level + 1)), ' ') : "";
  const std::string end_pad = indent > 0 ? std::string(static_cast<std::size_t>(indent * level), ' ') : "";
  const char* nl = indent > 0 ? "\n" : "";
  switch (v.type()) {
    case JsonValue::Type::Null: *out += "null"; break;
    case JsonValue::Type::Bool: *out += v.flag() ? "true" : "false"; break;
    case JsonValue::Type::Number: {
      const double d = v.num();
      char b[40];
      if (std::fabs(d - std::round(d)) < 1e-9 && std::fabs(d) < 1e15) {
        std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(std::llround(d)));
      } else {
        std::snprintf(b, sizeof(b), "%.6g", d);
      }
      *out += b;
      break;
    }
    case JsonValue::Type::String: dump_string(v.str(), out); break;
    case JsonValue::Type::Array: {
      if (v.items().empty()) {
        *out += "[]";
        break;
      }
      // Short scalar arrays stay on one line.
      bool flat = v.items().size() <= 4;
      for (const JsonValue& x : v.items()) flat = flat && !x.is_array() && !x.is_object();
      *out += "[";
      for (std::size_t k = 0; k < v.items().size(); ++k) {
        if (k) *out += flat ? ", " : ",";
        if (!flat) *out += nl + pad;
        dump_value(v.items()[k], indent, level + 1, out);
      }
      if (!flat) *out += nl + end_pad;
      *out += "]";
      break;
    }
    case JsonValue::Type::Object: {
      if (v.members().empty()) {
        *out += "{}";
        break;
      }
      *out += "{";
      for (std::size_t k = 0; k < v.members().size(); ++k) {
        if (k) *out += ",";
        *out += nl + pad;
        dump_string(v.members()[k].first, out);
        *out += indent > 0 ? ": " : ":";
        dump_value(v.members()[k].second, indent, level + 1, out);
      }
      *out += nl + end_pad;
      *out += "}";
      break;
    }
  }
}

const JsonValue& null_value() {
  static const JsonValue n;
  return n;
}

}  // namespace

JsonValue JsonValue::number(double v) {
  JsonValue j;
  j.type_ = Type::Number;
  j.num_ = v;
  return j;
}
JsonValue JsonValue::boolean(bool v) {
  JsonValue j;
  j.type_ = Type::Bool;
  j.bool_ = v;
  return j;
}
JsonValue JsonValue::string(std::string v) {
  JsonValue j;
  j.type_ = Type::String;
  j.str_ = std::move(v);
  return j;
}
JsonValue JsonValue::array() {
  JsonValue j;
  j.type_ = Type::Array;
  return j;
}
JsonValue JsonValue::object() {
  JsonValue j;
  j.type_ = Type::Object;
  return j;
}

bool JsonValue::parse(std::string_view text, JsonValue* out, std::string* error) {
  Parser p{text};
  JsonValue v;
  if (!p.value(&v)) {
    if (error) *error = p.err;
    return false;
  }
  p.ws();
  if (p.i != text.size()) {
    p.fail("trailing characters");
    if (error) *error = p.err;
    return false;
  }
  *out = std::move(v);
  return true;
}

std::string JsonValue::dump(int indent) const {
  std::string out;
  dump_value(*this, indent, 0, &out);
  return out;
}

const JsonValue& JsonValue::get(const std::string& key) const {
  for (const auto& [k, v] : obj_) {
    if (k == key) return v;
  }
  return null_value();
}

bool JsonValue::has(const std::string& key) const {
  for (const auto& kv : obj_) {
    if (kv.first == key) return true;
  }
  return false;
}

void JsonValue::set(const std::string& key, JsonValue v) {
  type_ = Type::Object;
  for (auto& kv : obj_) {
    if (kv.first == key) {
      kv.second = std::move(v);
      return;
    }
  }
  obj_.emplace_back(key, std::move(v));
}

}  // namespace ai
}  // namespace ts
