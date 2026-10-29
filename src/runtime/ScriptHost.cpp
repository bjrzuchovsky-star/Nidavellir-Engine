#include "profiler/Profiler.h"
#include "runtime/ScriptHost.h"

#include "runtime/GraphScript.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <mutex>

namespace ts {
namespace tombstone {
namespace runtime {

namespace {
constexpr const char* kOrder[ScriptHost::kLanguages] = {"lua", "graph", "cpp", "cs"};

std::size_t slot_of(const std::string& lang) {
  for (std::size_t i = 0; i < ScriptHost::kLanguages; ++i) {
    if (lang == kOrder[i]) return i;
  }
  return ScriptHost::kLanguages;
}

std::map<std::string, ScriptLanguageFactory>& registry() {
  static std::map<std::string, ScriptLanguageFactory> r;
  return r;
}
std::mutex& registry_mutex() {
  static std::mutex m;
  return m;
}
}  // namespace

// Built-in languages (GraphScript.cpp, NativeScript.cpp, CSharpScript.cpp).
void register_builtin_script_languages();

void register_script_language(const std::string& id, ScriptLanguageFactory factory) {
  std::lock_guard<std::mutex> lock(registry_mutex());
  registry()[id] = std::move(factory);
}

ScriptHost::ScriptHost(ScriptWorld& world, ScriptLibrary& library, std::string project_dir)
    : world_(world), library_(library), project_dir_(std::move(project_dir)) {
  static std::once_flag builtins;
  std::call_once(builtins, [] { register_builtin_script_languages(); });
  auto lua = std::make_unique<LuaScriptHost>(world_, library_, project_dir_);
  lua_ = lua.get();
  hosts_[0] = std::move(lua);
}

ScriptHost::~ScriptHost() = default;

ScriptLanguageHost* ScriptHost::host_for(const std::string& lang) {
  const std::size_t i = slot_of(lang);
  if (i >= kLanguages) return nullptr;
  if (!hosts_[i]) {
    ScriptLanguageFactory f;
    {
      std::lock_guard<std::mutex> lock(registry_mutex());
      const auto it = registry().find(lang);
      if (it != registry().end()) f = it->second;
    }
    if (!f) return nullptr;
    hosts_[i] = f(world_, library_, project_dir_);
  }
  return hosts_[i].get();
}

ScriptLanguageHost* ScriptHost::language(const std::string& id) const {
  const std::size_t i = slot_of(id);
  return i < kLanguages ? hosts_[i].get() : nullptr;
}

const std::vector<ScriptHost::Slot>* ScriptHost::slots(std::uint64_t entity) const {
  const auto it = owner_.find(entity);
  return it == owner_.end() ? nullptr : &it->second;
}

bool ScriptHost::attach(const Entity2D& e) {
  if (!e.script || owner_.count(e.id)) return false;
  const std::vector<ScriptData> stack = script_stack(e);
  std::vector<Slot>& mine = owner_[e.id];
  bool all = true;
  bool any = false;
  for (std::size_t k = 0; k < stack.size(); ++k) {
    const ScriptData& sd = stack[k];
    if (!sd.enabled) continue;  // switched off in the Inspector
    std::string lang = script_language(sd.path);
    if (lang.empty()) lang = "lua";  // empty / odd paths: Lua reports them
    ScriptLanguageHost* h = host_for(lang);
    if (!h) {
      LogEntry le;
      le.level = LogLevel::Error;
      le.channel = "script";
      le.text = "no " + lang + " script host in this build -- \"" + e.name + "\" (" + sd.path + ")";
      le.file = sd.path;
      le.entity = e.id;
      le.tick = world_.script_tick();
      world_.script_log(std::move(le));
      all = false;
      continue;
    }
    const int index = static_cast<int>(k);
    mine.push_back({slot_of(lang), index});
    depth_ = std::max(depth_, index + 1);
    Entity2D one = e;  // the language host sees this slot's script only
    one.script = sd;
    one.extra_scripts.clear();
    const bool ok = h->attach(one, index);
    all = all && ok;
    any = any || ok;
  }
  if (mine.empty()) owner_.erase(e.id);
  return any && all;
}

void ScriptHost::detach(std::uint64_t entity) {
  const auto it = owner_.find(entity);
  if (it == owner_.end()) return;
  bool seen[kLanguages] = {};
  for (const Slot& s : it->second) {
    if (seen[s.lang] || !hosts_[s.lang]) continue;
    seen[s.lang] = true;
    hosts_[s.lang]->detach(entity);
  }
  owner_.erase(it);
}

bool ScriptHost::has(std::uint64_t entity) const {
  const auto* list = slots(entity);
  if (!list) return false;
  for (const Slot& s : *list) {
    if (hosts_[s.lang] && hosts_[s.lang]->has(entity, s.index)) return true;
  }
  return false;
}

bool ScriptHost::running(std::uint64_t entity) const {
  const auto* list = slots(entity);
  if (!list) return false;
  for (const Slot& s : *list) {
    if (hosts_[s.lang] && hosts_[s.lang]->running(entity, s.index)) return true;
  }
  return false;
}

bool ScriptHost::running(std::uint64_t entity, int index) const {
  const auto* list = slots(entity);
  if (!list) return false;
  for (const Slot& s : *list) {
    if (s.index == index) return hosts_[s.lang] && hosts_[s.lang]->running(entity, s.index);
  }
  return false;
}

std::size_t ScriptHost::script_count(std::uint64_t entity) const {
  const auto* list = slots(entity);
  return list ? list->size() : 0;
}

bool ScriptHost::has_hook(std::uint64_t entity, const char* hook) {
  const auto* list = slots(entity);
  if (!list) return false;
  for (const Slot& s : *list) {
    ScriptLanguageHost* h = hosts_[s.lang].get();
    if (h && h->running(entity, s.index) && h->has_hook(entity, s.index, hook)) return true;
  }
  return false;
}

std::size_t ScriptHost::instance_count() const {
  std::size_t n = 0;
  for (const auto& h : hosts_) n += h ? h->instance_count() : 0;
  return n;
}
std::size_t ScriptHost::timer_count() const { return lua_->timer_count(); }
std::size_t ScriptHost::memory_used() const { return lua_->memory_used(); }
std::size_t ScriptHost::ui_handler_count() const { return lua_->ui_handler_count(); }

void ScriptHost::start_pending() {
  for (int k = 0; k < depth_; ++k) {
    for (auto& h : hosts_) if (h) h->start_pending(k);
  }
}

// Per-object events: the object's scripts in stack order. Copy the list
// first: a hook may destroy the object (detach) mid-way.
void ScriptHost::collide(std::uint64_t self, std::uint64_t other, int phase) {
  const auto* list = slots(self);
  if (!list) return;
  const std::vector<Slot> order = *list;
  for (const Slot& s : order) {
    if (!owner_.count(self)) break;
    if (ScriptLanguageHost* h = hosts_[s.lang].get()) h->collide(self, s.index, other, phase);
  }
}

void ScriptHost::trigger(bool enter, std::uint64_t trigger, std::uint64_t other) {
  const auto* list = slots(trigger);
  if (!list) return;
  const std::vector<Slot> order = *list;
  for (const Slot& s : order) {
    if (!owner_.count(trigger)) break;
    if (ScriptLanguageHost* h = hosts_[s.lang].get()) h->trigger(enter, trigger, s.index, other);
  }
}

void ScriptHost::interact(std::uint64_t target, std::uint64_t rider) {
  const auto* list = slots(target);
  if (!list) return;
  const std::vector<Slot> order = *list;
  for (const Slot& s : order) {
    if (!owner_.count(target)) break;
    if (ScriptLanguageHost* h = hosts_[s.lang].get()) h->interact(target, s.index, rider);
  }
}

void ScriptHost::ui_event(const std::string& widget, bool change, double value,
                          const std::string& text) {
  for (auto& h : hosts_) if (h) h->ui_event(widget, change, value, text);
}

void ScriptHost::hand_event(std::uint64_t target, const char* hook, const std::string& text,
                            double number) {
  const auto* list = slots(target);
  if (!list) return;
  const std::vector<Slot> order = *list;
  for (const Slot& s : order) {
    if (!owner_.count(target)) break;
    if (ScriptLanguageHost* h = hosts_[s.lang].get()) h->hand_event(target, s.index, hook, text, number);
  }
}

void ScriptHost::broadcast(const char* hook, const std::string& text, double number) {
  for (int k = 0; k < depth_; ++k) {
    for (auto& h : hosts_) if (h) h->broadcast(k, hook, text, number);
  }
}

void ScriptHost::run_timers(std::uint64_t tick) {
  for (auto& h : hosts_) if (h) h->run_timers(tick);
}

namespace {
const char* tick_scope(const char* lang) {
  const std::string l = lang ? lang : "";
  if (l == "lua") return "scripts.lua";
  if (l == "graph") return "scripts.graph";
  if (l == "cpp" || l == "c++") return "scripts.cpp";
  if (l == "cs" || l == "c#" || l == "csharp") return "scripts.cs";
  return "scripts.other";
}
}  // namespace

void ScriptHost::tick(double dt) {
  for (int k = 0; k < depth_; ++k) {
    for (auto& h : hosts_) {
      if (!h) continue;
      TS_PROFILE_SCOPE(tick_scope(h->language()));
      h->tick(dt, k);
    }
  }
}

void ScriptHost::fixed_tick(double dt) {
  for (int k = 0; k < depth_; ++k)
    for (auto& h : hosts_)
      if (h) h->fixed_tick(dt, k);
}

void ScriptHost::late_tick(double dt) {
  for (int k = 0; k < depth_; ++k)
    for (auto& h : hosts_)
      if (h) h->late_tick(dt, k);
}

int ScriptHost::reload_changed() {
  // Lua + graphs read through the shared library: refresh it once and hand
  // each changed file to its language. Compiled languages watch their own.
  int n = 0;
  for (const std::string& rel : library_.refresh(project_dir_)) {
    ScriptLanguageHost* h = language(script_language(rel));
    if (h && h->uses(rel)) {
      h->reload(rel);
      ++n;
    }
  }
  for (std::size_t i = 2; i < kLanguages; ++i) n += hosts_[i] ? hosts_[i]->reload_changed() : 0;
  return n;
}

bool ScriptHost::reload(const std::string& rel) {
  ScriptLanguageHost* h = language(script_language(rel));
  return h && h->reload(rel);
}

bool ScriptHost::describe(const std::string& source, const std::string& rel,
                          std::vector<ScriptProp>* defaults, std::string* error) {
  const std::string lang = script_language(rel);
  if (lang == "lua" || lang.empty()) {
    return LuaScriptHost::describe(source, rel, defaults, error);
  }
  if (defaults) defaults->clear();
  if (lang == "graph") return describe_graph_props(source, rel, defaults, error);
  if (lang == "cpp" || lang == "cs") {
    if (defaults) *defaults = describe_compiled_props(source, lang == "cs");
    return true;
  }
  return true;
}

// C++ `prop("reach", 24.0)` / C# `Prop("reach", 24.0)` calls with a literal
// default: number, "text" or true / false. Sorted by name; the first call
// of a name wins.
std::string attr_quoted(const std::string& line, const char* key) {
  const std::string pat = std::string(key) + "(\"";
  const std::size_t p = line.find(pat);
  if (p == std::string::npos) return {};
  const std::size_t a = p + pat.size();
  const std::size_t b = line.find('"', a);
  if (b == std::string::npos) return {};
  return line.substr(a, b - a);
}

void scan_serialized_fields(const std::string& src, bool csharp, std::vector<ScriptProp>& out) {
  std::map<std::string, std::string> enums;
  for (std::size_t i = 0; (i = src.find("enum ", i)) != std::string::npos; i += 5) {
    std::size_t n = i + 5;
    while (n < src.size() && std::isspace(static_cast<unsigned char>(src[n]))) ++n;
    std::size_t ne = n;
    while (ne < src.size() && (std::isalnum(static_cast<unsigned char>(src[ne])) || src[ne] == '_')) ++ne;
    const std::string name = src.substr(n, ne - n);
    const std::size_t brace = src.find('{', ne);
    const std::size_t end = src.find('}', brace == std::string::npos ? ne : brace);
    if (brace == std::string::npos || end == std::string::npos || brace > i + 80) continue;
    std::string opts;
    std::string body = src.substr(brace + 1, end - brace - 1);
    std::string cur;
    for (char c : body) {
      if (c == ',' || c == '\n') {
        while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.front()))) cur.erase(cur.begin());
        while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) cur.pop_back();
        const std::size_t sp = cur.find_first_of("= \t");
        if (sp != std::string::npos) cur = cur.substr(0, sp);
        if (!cur.empty()) opts += opts.empty() ? cur : "," + cur;
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) {
      while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.front()))) cur.erase(cur.begin());
      const std::size_t sp = cur.find_first_of("= \t");
      if (sp != std::string::npos) cur = cur.substr(0, sp);
      if (!cur.empty()) opts += opts.empty() ? cur : "," + cur;
    }
    if (!name.empty()) enums[name] = opts;
  }
  std::istringstream in(src);
  std::string line;
  std::string header, tip;
  bool range = false;
  float r0 = 0, r1 = 0;
  bool serialize = false;
  auto known = [&](const std::string& name) {
    return std::any_of(out.begin(), out.end(), [&](const ScriptProp& p) { return p.name == name; });
  };
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string h = attr_quoted(line, "Header");
    const std::string t = attr_quoted(line, "Tooltip");
    if (!h.empty()) header = h;
    if (!t.empty()) tip = t;
    if (line.find("[Range(") != std::string::npos || line.find("Range(") != std::string::npos) {
      float a = 0, b = 0;
      const std::size_t p = line.find("Range(");
      if (p != std::string::npos && std::sscanf(line.c_str() + p, "Range(%f, %f", &a, &b) == 2) {
        range = true;
        r0 = a;
        r1 = b;
      }
    }
    if (line.find("SerializeField") != std::string::npos) serialize = true;
    const std::size_t eq = line.find('=');
    const std::size_t paren = line.find('(');
    const bool call_before_value = paren != std::string::npos && (eq == std::string::npos || paren < eq);
    const bool decl = eq == std::string::npos && line.find(';') != std::string::npos && !call_before_value;
    if ((eq == std::string::npos && !decl) || call_before_value) {
      if (line.find('[') == std::string::npos && line.find("//") == std::string::npos) {
        header.clear();
        tip.clear();
        range = false;
        serialize = false;
      }
      continue;
    }
    std::string left = eq == std::string::npos ? line.substr(0, line.find(';')) : line.substr(0, eq);
    std::string right = eq == std::string::npos ? std::string() : line.substr(eq + 1);
    const bool is_public = left.find("public") != std::string::npos;
    if (csharp && !is_public && !serialize) {
      header.clear();
      tip.clear();
      range = false;
      serialize = false;
      continue;
    }
    while (!left.empty() && std::isspace(static_cast<unsigned char>(left.back()))) left.pop_back();
    std::size_t ns = left.find_last_of(" \t");
    if (ns == std::string::npos) continue;
    const std::string name = left.substr(ns + 1);
    std::string type = left.substr(0, ns);
    while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back()))) type.pop_back();
    ns = type.find_last_of(" \t");
    type = ns == std::string::npos ? type : type.substr(ns + 1);
    if (name.empty() || known(name)) {
      header.clear();
      tip.clear();
      range = false;
      serialize = false;
      continue;
    }
    ScriptProp p;
    p.name = name;
    p.header = header;
    p.tooltip = tip;
    p.has_range = range;
    p.range_min = r0;
    p.range_max = r1;
    while (!right.empty() && std::isspace(static_cast<unsigned char>(right.front()))) right.erase(right.begin());
    if (!right.empty() && right.back() == ';') right.pop_back();
    if (type == "bool") p.value = ScriptValue::of_bool(right.find("true") != std::string::npos);
    else if (type == "string" || type == "std::string") {
      const std::size_t q = right.find('"');
      const std::size_t q2 = right.find('"', q == std::string::npos ? 0 : q + 1);
      p.value = ScriptValue::of_text(q != std::string::npos && q2 != std::string::npos ? right.substr(q + 1, q2 - q - 1) : "");
    } else if (type == "Vector3" || type == "ts::Vector3") {
      float x = 0, y = 0, z = 0;
      const std::size_t paren = right.find_first_of("({");
      if (paren != std::string::npos) {
        const char* s = right.c_str() + paren + 1;
        std::sscanf(s, "%f, %f, %f", &x, &y, &z);
      }
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.6g,%.6g,%.6g", x, y, z);
      p.value = ScriptValue::of_text(buf);
      p.widget = "vector";
    } else if (type == "Color" || type == "Colour") {
      float r = 1, g = 1, b = 1, a = 1;
      const std::size_t paren = right.find('(');
      if (paren != std::string::npos) std::sscanf(right.c_str() + paren, "(%f, %f, %f, %f", &r, &g, &b, &a);
      char buf[80];
      std::snprintf(buf, sizeof(buf), "%.6g,%.6g,%.6g,%.6g", r, g, b, a);
      p.value = ScriptValue::of_text(buf);
      p.widget = "color";
    } else if (type == "Entity" || type == "ts::Entity" || type == "EntityRef") {
      p.value = ScriptValue::of_text("");
      p.widget = "ref";
    } else if (enums.count(type)) {
      std::string lit = right;
      const std::size_t dot = lit.rfind('.');
      if (dot != std::string::npos) lit = lit.substr(dot + 1);
      while (!lit.empty() && (lit.back() == ';' || std::isspace(static_cast<unsigned char>(lit.back())))) lit.pop_back();
      p.value = ScriptValue::of_text(lit);
      p.widget = "enum";
      p.options = enums[type];
    } else {
      char* end = nullptr;
      const double v = std::strtod(right.c_str(), &end);
      if (!end || end == right.c_str()) {
        header.clear();
        tip.clear();
        range = false;
        serialize = false;
        continue;
      }
      p.value = ScriptValue::of_number(v);
    }
    out.push_back(std::move(p));
    header.clear();
    tip.clear();
    range = false;
    serialize = false;
  }
}

std::vector<ScriptProp> describe_compiled_props(const std::string& src, bool csharp) {
  std::vector<ScriptProp> out;
  const std::string fn = csharp ? "Prop" : "prop";
  auto ident = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
  };
  auto skip_ws = [&](std::size_t i) {
    while (i < src.size() && std::isspace(static_cast<unsigned char>(src[i]))) ++i;
    return i;
  };
  for (std::size_t at = src.find(fn); at != std::string::npos; at = src.find(fn, at + 1)) {
    if (at > 0 && ident(src[at - 1])) continue;  // part of a longer name
    std::size_t i = at + fn.size();
    if (i < src.size() && ident(src[i])) continue;
    i = skip_ws(i);
    if (i >= src.size() || src[i] != '(') continue;
    i = skip_ws(i + 1);
    if (i >= src.size() || src[i] != '"') continue;
    const std::size_t name_end = src.find('"', i + 1);
    if (name_end == std::string::npos) break;
    ScriptProp p;
    p.name = src.substr(i + 1, name_end - i - 1);
    i = skip_ws(name_end + 1);
    if (i >= src.size() || src[i] != ',') continue;
    i = skip_ws(i + 1);
    if (i >= src.size()) break;
    bool ok = false;
    if (src[i] == '"') {
      std::string text;
      std::size_t j = i + 1;
      for (; j < src.size() && src[j] != '"'; ++j) {
        if (src[j] == '\\' && j + 1 < src.size()) ++j;
        text += src[j];
      }
      p.value = ScriptValue::of_text(text);
      ok = j < src.size();
    } else if (src.compare(i, 4, "true") == 0 || src.compare(i, 5, "false") == 0) {
      p.value = ScriptValue::of_bool(src[i] == 't');
      ok = true;
    } else {
      std::size_t j = i;
      if (j < src.size() && (src[j] == '-' || src[j] == '+')) ++j;
      const std::size_t digits = j;
      while (j < src.size() &&
             (std::isdigit(static_cast<unsigned char>(src[j])) || src[j] == '.' || src[j] == 'e' ||
              src[j] == 'E')) {
        ++j;
      }
      if (j > digits) {
        char* end = nullptr;
        const std::string lit = src.substr(i, j - i);
        const double v = std::strtod(lit.c_str(), &end);
        ok = end && *end == '\0';
        p.value = ScriptValue::of_number(v);
      }
    }
    if (!ok || p.name.empty()) continue;
    const bool seen = std::any_of(out.begin(), out.end(),
                                  [&](const ScriptProp& q) { return q.name == p.name; });
    if (!seen) out.push_back(std::move(p));
  }
  scan_serialized_fields(src, csharp, out);
  std::sort(out.begin(), out.end(),
            [](const ScriptProp& a, const ScriptProp& b) { return a.name < b.name; });
  return out;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
