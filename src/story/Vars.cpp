#include "story/Vars.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace ts {
namespace tombstone {
namespace story {

namespace {

std::string trim(const std::string& s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool is_number(const std::string& t, double* out) {
  if (t.empty()) return false;
  char* end = nullptr;
  const double d = std::strtod(t.c_str(), &end);
  if (end != t.c_str() + t.size()) return false;
  *out = d;
  return true;
}

std::vector<std::string> split_on(const std::string& s, const std::string& sep) {
  std::vector<std::string> out;
  std::size_t start = 0;
  for (;;) {
    const std::size_t p = s.find(sep, start);
    if (p == std::string::npos) {
      out.push_back(s.substr(start));
      return out;
    }
    out.push_back(s.substr(start, p - start));
    start = p + sep.size();
  }
}

bool valid_name(const std::string& n) {
  if (n.empty()) return false;
  for (char c : n) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '/' ||
          c == ':' || c == '-')) {
      return false;
    }
  }
  return true;
}

// Left side of a comparison: a variable, quest:<id> or obj:<q>/<o>.
VarValue lhs_value(const std::string& name, const ConditionEnv& env) {
  if (name.rfind("quest:", 0) == 0) {
    return VarValue::string(env.quest_status ? env.quest_status(name.substr(6)) : "inactive");
  }
  if (name.rfind("obj:", 0) == 0) {
    const std::string rest = name.substr(4);
    const std::size_t slash = rest.find('/');
    if (slash == std::string::npos || !env.objective) return VarValue::number(0);
    return VarValue::number(env.objective(rest.substr(0, slash), rest.substr(slash + 1)));
  }
  return env.vars ? env.vars->get(name) : VarValue::boolean(false);
}

const char* kOps[] = {">=", "<=", "==", "!=", ">", "<"};

bool eval_term(const std::string& term_in, const ConditionEnv* env, bool* result, std::string* error) {
  std::string term = trim(term_in);
  bool negate = false;
  while (!term.empty() && term[0] == '!' && (term.size() < 2 || term[1] != '=')) {
    negate = !negate;
    term = trim(term.substr(1));
  }
  std::string op;
  std::size_t at = std::string::npos;
  for (const char* o : kOps) {
    const std::size_t p = term.find(o);
    if (p != std::string::npos) {
      op = o;
      at = p;
      break;
    }
  }
  if (op.empty()) {
    if (!valid_name(term)) {
      if (error) *error = "bad condition term \"" + term_in + "\"";
      return false;
    }
    if (env) *result = lhs_value(term, *env).truthy() != negate;
    return true;
  }
  const std::string name = trim(term.substr(0, at));
  const std::string rhs = trim(term.substr(at + op.size()));
  if (!valid_name(name) || rhs.empty() || std::string("=<>!&|").find(rhs[0]) != std::string::npos) {
    if (error) *error = "bad comparison \"" + term_in + "\"";
    return false;
  }
  if (!env) return true;
  const VarValue l = lhs_value(name, *env);
  const VarValue r = VarValue::parse(rhs);
  bool v = false;
  if (op == "==") v = l == r;
  else if (op == "!=") v = !(l == r);
  else {
    const double a = l.num(), b = r.num();
    v = op == ">=" ? a >= b : op == "<=" ? a <= b : op == ">" ? a > b : a < b;
  }
  *result = v != negate;
  return true;
}

bool eval_impl(const std::string& cond, const ConditionEnv* env, bool* out, std::string* error) {
  if (trim(cond).empty()) {
    *out = true;
    return true;
  }
  bool any = false;
  for (const std::string& alt : split_on(cond, "||")) {
    bool all = true;
    for (const std::string& term : split_on(alt, "&&")) {
      bool r = false;
      if (!eval_term(term, env, &r, error)) return false;
      all = all && r;
    }
    any = any || all;
  }
  *out = any;
  return true;
}

}  // namespace

VarValue VarValue::parse(const std::string& text_in) {
  const std::string t = trim(text_in);
  if (t == "true") return boolean(true);
  if (t == "false") return boolean(false);
  double d = 0;
  if (is_number(t, &d)) return number(d);
  if (t.size() >= 2 && (t.front() == '\'' || t.front() == '"') && t.back() == t.front()) {
    return string(t.substr(1, t.size() - 2));
  }
  return string(t);
}

bool VarValue::truthy() const {
  switch (kind) {
    case Kind::Bool: return b;
    case Kind::Number: return n != 0.0;
    case Kind::String: return !s.empty() && s != "inactive";
  }
  return false;
}

double VarValue::num() const {
  switch (kind) {
    case Kind::Bool: return b ? 1.0 : 0.0;
    case Kind::Number: return n;
    case Kind::String: {
      double d = 0;
      return is_number(s, &d) ? d : 0.0;
    }
  }
  return 0.0;
}

std::string VarValue::str() const {
  switch (kind) {
    case Kind::Bool: return b ? "true" : "false";
    case Kind::Number: {
      if (std::floor(n) == n && std::fabs(n) < 1e15) return std::to_string(static_cast<long long>(n));
      std::ostringstream o;
      o << n;
      return o.str();
    }
    case Kind::String: return s;
  }
  return {};
}

bool VarValue::operator==(const VarValue& o) const {
  if (kind == Kind::Number || o.kind == Kind::Number) {
    if (kind == Kind::String || o.kind == Kind::String) return str() == o.str();
    return num() == o.num();
  }
  if (kind == Kind::Bool && o.kind == Kind::Bool) return b == o.b;
  return str() == o.str();
}

VarValue Vars::get(const std::string& k) const {
  const auto it = m_.find(k);
  return it == m_.end() ? VarValue::boolean(false) : it->second;
}

double Vars::add(const std::string& k, double n) {
  const double v = get(k).num() + n;
  m_[k] = VarValue::number(v);
  return v;
}

json::Value Vars::to_json() const {
  json::Value o = json::Value::object();
  for (const auto& [k, v] : m_) {
    switch (v.kind) {
      case VarValue::Kind::Bool: o.set(k, json::Value::boolean(v.b)); break;
      case VarValue::Kind::Number: o.set(k, json::Value::number(v.n)); break;
      case VarValue::Kind::String: o.set(k, json::Value::string(v.s)); break;
    }
  }
  return o;
}

void Vars::from_json(const json::Value& v) {
  m_.clear();
  for (const auto& [k, x] : v.members()) {
    if (x.is_bool()) m_[k] = VarValue::boolean(x.as_bool());
    else if (x.is_number()) m_[k] = VarValue::number(x.as_number());
    else if (x.is_string()) m_[k] = VarValue::string(x.as_string());
  }
}

std::string Vars::format(const std::string& text) const {
  std::string out;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '{') {
      const std::size_t close = text.find('}', i);
      if (close != std::string::npos) {
        const std::string name = text.substr(i + 1, close - i - 1);
        if (has(name)) {
          out += get(name).str();
          i = close;
          continue;
        }
      }
    }
    out += text[i];
  }
  return out;
}

bool eval_condition(const std::string& cond, const ConditionEnv& env, std::string* error) {
  bool r = false;
  if (!eval_impl(cond, &env, &r, error)) return false;
  return r;
}

bool check_condition(const std::string& cond, std::string* error) {
  bool r = false;
  return eval_impl(cond, nullptr, &r, error);
}

bool parse_action(const std::string& text, Action* out, std::string* error) {
  std::istringstream in(text);
  Action a;
  in >> a.verb;
  std::string w;
  while (in >> w) {
    // 'quoted words' stay together.
    if (!w.empty() && (w[0] == '\'' || w[0] == '"')) {
      const char q = w[0];
      while (w.size() < 2 || w.back() != q) {
        std::string more;
        if (!(in >> more)) break;
        w += " " + more;
      }
      if (w.size() >= 2 && w.back() == q) w = w.substr(1, w.size() - 2);
    }
    a.args.push_back(w);
  }
  auto bad = [&](const std::string& why) {
    if (error) *error = "action \"" + text + "\": " + why;
    return false;
  };
  if (a.verb == "set") {
    if (a.args.size() != 2) return bad("set k v");
  } else if (a.verb == "add") {
    double d = 0;
    if (a.args.size() != 2 || !is_number(a.args[1], &d)) return bad("add k n");
  } else if (a.verb == "quest") {
    if (a.args.empty()) return bad("quest start|complete|fail|progress ...");
    const std::string& s = a.args[0];
    if (s == "start" || s == "complete" || s == "fail" || s == "track") {
      if (a.args.size() != 2) return bad("quest " + s + " id");
    } else if (s == "progress") {
      double d = 0;
      if (a.args.size() < 3 || a.args.size() > 4 || (a.args.size() == 4 && !is_number(a.args[3], &d))) {
        return bad("quest progress q o [n]");
      }
    } else {
      return bad("unknown quest verb " + s);
    }
  } else if (a.verb == "event") {
    if (a.args.empty() || a.args.size() > 2) return bad("event name [arg]");
  } else if (a.verb == "save") {
    if (a.args.size() > 1) return bad("save [slot]");
  } else if (a.verb == "end") {
    if (!a.args.empty()) return bad("end takes nothing");
  } else {
    return bad("unknown verb \"" + a.verb + "\"");
  }
  *out = std::move(a);
  return true;
}

}  // namespace story
}  // namespace tombstone
}  // namespace ts
