#include "profiler/Profiler.h"
#include "runtime/ScriptHost.h"

#include "runtime/GraphScript.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
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
  std::sort(out.begin(), out.end(),
            [](const ScriptProp& a, const ScriptProp& b) { return a.name < b.name; });
  return out;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
