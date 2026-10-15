#include "store/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ts {
namespace tombstone {
namespace store {

namespace {

const Json& null_json() {
  static const Json n;
  return n;
}

struct Parser {
  std::string_view s;
  std::size_t i = 0;
  std::string err;
  int depth = 0;

  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  bool fail(const std::string& what) {
    if (err.empty()) err = what + " at offset " + std::to_string(i);
    return false;
  }
  bool lit(const char* word) {
    const std::size_t n = std::char_traits<char>::length(word);
    if (s.substr(i, n) != word) return fail(std::string("expected ") + word);
    i += n;
    return true;
  }
  static void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(unsigned* out) {
    if (i + 4 > s.size()) return fail("short \\u escape");
    unsigned v = 0;
    for (int k = 0; k < 4; ++k) {
      const char c = s[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
      else return fail("bad \\u escape");
    }
    *out = v;
    return true;
  }
  bool string(std::string* out) {
    if (i >= s.size() || s[i] != '"') return fail("expected string");
    ++i;
    while (i < s.size()) {
      const char c = s[i++];
      if (c == '"') return true;
      if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
      if (c != '\\') {
        *out += c;
        continue;
      }
      if (i >= s.size()) break;
      const char e = s[i++];
      switch (e) {
        case '"': *out += '"'; break;
        case '\\': *out += '\\'; break;
        case '/': *out += '/'; break;
        case 'b': *out += '\b'; break;
        case 'f': *out += '\f'; break;
        case 'n': *out += '\n'; break;
        case 'r': *out += '\r'; break;
        case 't': *out += '\t'; break;
        case 'u': {
          unsigned cp = 0;
          if (!hex4(&cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            i += 2;
            unsigned lo = 0;
            if (!hex4(&lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          utf8(*out, cp);
          break;
        }
        default: return fail("bad escape");
      }
    }
    return fail("unterminated string");
  }
  bool number(Json* out) {
    const std::size_t start = i;
    if (i < s.size() && s[i] == '-') ++i;
    bool digits = false;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                            s[i] == 'E' || s[i] == '+' || s[i] == '-')) {
      digits = true;
      ++i;
    }
    if (!digits) return fail("expected value");
    const std::string text(s.substr(start, i - start));
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(v)) return fail("bad number");
    *out = Json(v);
    return true;
  }
  bool value(Json* out) {
    if (++depth > 64) return fail("nested too deep");
    ws();
    if (i >= s.size()) return fail("unexpected end");
    const char c = s[i];
    bool ok = true;
    if (c == '{') {
      ++i;
      *out = Json::object();
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
          Json v;
          if (!value(&v)) return false;
          out->set(key, std::move(v));
          ws();
          if (i < s.size() && s[i] == ',') { ++i; continue; }
          if (i < s.size() && s[i] == '}') { ++i; break; }
          return fail("expected ',' or '}'");
        }
      }
    } else if (c == '[') {
      ++i;
      *out = Json::array();
      ws();
      if (i < s.size() && s[i] == ']') {
        ++i;
      } else {
        for (;;) {
          Json v;
          if (!value(&v)) return false;
          out->push(std::move(v));
          ws();
          if (i < s.size() && s[i] == ',') { ++i; continue; }
          if (i < s.size() && s[i] == ']') { ++i; break; }
          return fail("expected ',' or ']'");
        }
      }
    } else if (c == '"') {
      std::string str;
      ok = string(&str);
      *out = Json(std::move(str));
    } else if (c == 't') {
      ok = lit("true");
      *out = Json(true);
    } else if (c == 'f') {
      ok = lit("false");
      *out = Json(false);
    } else if (c == 'n') {
      ok = lit("null");
      *out = Json();
    } else {
      ok = number(out);
    }
    --depth;
    return ok;
  }
};

void escape_into(std::string& out, const std::string& v) {
  out += '"';
  for (unsigned char c : v) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

}  // namespace

bool Json::parse(std::string_view text, Json* out, std::string* error) {
  Parser p{text};
  Json v;
  bool ok = p.value(&v);
  if (ok) {
    p.ws();
    if (p.i != text.size()) ok = p.fail("trailing characters");
  }
  if (!ok) {
    if (error) *error = p.err;
    return false;
  }
  if (out) *out = std::move(v);
  return true;
}

const std::string& Json::as_string() const {
  static const std::string empty;
  return is_string() ? str_ : empty;
}

std::size_t Json::size() const {
  if (is_array()) return arr_.size();
  if (is_object()) return obj_.size();
  return 0;
}

const Json& Json::at(std::size_t i) const {
  return is_array() && i < arr_.size() ? arr_[i] : null_json();
}

void Json::push(Json v) {
  if (!is_array()) *this = array();
  arr_.push_back(std::move(v));
}

bool Json::has(const std::string& key) const {
  if (!is_object()) return false;
  for (const auto& m : obj_) {
    if (m.first == key) return true;
  }
  return false;
}

const Json& Json::get(const std::string& key) const {
  if (is_object()) {
    for (const auto& m : obj_) {
      if (m.first == key) return m.second;
    }
  }
  return null_json();
}

Json& Json::set(const std::string& key, Json v) {
  if (!is_object()) *this = object();
  for (auto& m : obj_) {
    if (m.first == key) {
      m.second = std::move(v);
      return m.second;
    }
  }
  obj_.emplace_back(key, std::move(v));
  return obj_.back().second;
}

std::vector<std::string> Json::strings_at(const std::string& key) const {
  std::vector<std::string> out;
  const Json& a = get(key);
  if (a.is_array()) {
    for (const Json& v : a.items()) {
      if (v.is_string() && !v.as_string().empty()) out.push_back(v.as_string());
    }
  } else if (a.is_string()) {
    // "a, b" is accepted too (the project.json "plugins" codec).
    std::string cur;
    auto flush = [&] {
      std::size_t b = 0, e = cur.size();
      while (b < e && cur[b] == ' ') ++b;
      while (e > b && cur[e - 1] == ' ') --e;
      if (e > b) out.push_back(cur.substr(b, e - b));
      cur.clear();
    };
    for (char c : a.as_string()) {
      if (c == ',') flush(); else cur += c;
    }
    flush();
  }
  return out;
}

void Json::dump_into(std::string& out, int indent, int depth) const {
  auto nl = [&](int d) {
    if (indent <= 0) return;
    out += '\n';
    out.append(static_cast<std::size_t>(indent * d), ' ');
  };
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: {
      char buf[40];
      if (std::floor(num_) == num_ && std::fabs(num_) < 1e15) {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num_));
      } else {
        std::snprintf(buf, sizeof(buf), "%.17g", num_);
      }
      out += buf;
      break;
    }
    case Type::String: escape_into(out, str_); break;
    case Type::Array:
      if (arr_.empty()) { out += "[]"; break; }
      out += '[';
      for (std::size_t k = 0; k < arr_.size(); ++k) {
        if (k) out += ',';
        nl(depth + 1);
        arr_[k].dump_into(out, indent, depth + 1);
      }
      nl(depth);
      out += ']';
      break;
    case Type::Object:
      if (obj_.empty()) { out += "{}"; break; }
      out += '{';
      for (std::size_t k = 0; k < obj_.size(); ++k) {
        if (k) out += ',';
        nl(depth + 1);
        escape_into(out, obj_[k].first);
        out += indent > 0 ? ": " : ":";
        obj_[k].second.dump_into(out, indent, depth + 1);
      }
      nl(depth);
      out += '}';
      break;
  }
}

std::string Json::dump(int indent) const {
  std::string out;
  dump_into(out, indent, 0);
  if (indent > 0) out += '\n';
  return out;
}

bool read_text_file(const std::string& path, std::string* out, std::string* error) {
  std::ifstream in(std::filesystem::path(path), std::ios::binary);
  if (!in) {
    if (error) *error = "could not open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

bool write_text_file(const std::string& path, const std::string& text, std::string* error) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "could not write " + path;
    return false;
  }
  out << text;
  if (!out) {
    if (error) *error = "failed writing " + path;
    return false;
  }
  return true;
}

bool read_json_file(const std::string& path, Json* out, std::string* error) {
  std::string text;
  if (!read_text_file(path, &text, error)) return false;
  std::string err;
  if (!Json::parse(text, out, &err)) {
    if (error) *error = path + ": " + err;
    return false;
  }
  return true;
}

}  // namespace store
}  // namespace tombstone
}  // namespace ts
