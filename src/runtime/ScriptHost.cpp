#include "profiler/Profiler.h"
#include "runtime/ScriptHost.h"

#include "runtime/GraphScript.h"

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

ScriptLanguageHost* ScriptHost::owner(std::uint64_t entity) const {
  const auto it = owner_.find(entity);
  return it == owner_.end() ? nullptr : hosts_[it->second].get();
}

bool ScriptHost::attach(const Entity2D& e) {
  if (!e.script) return false;
  std::string lang = script_language(e.script->path);
  if (lang.empty()) lang = "lua";  // empty / odd paths: Lua reports them
  ScriptLanguageHost* h = host_for(lang);
  if (!h) {
    LogEntry le;
    le.level = LogLevel::Error;
    le.channel = "script";
    le.text = "no " + lang + " script host in this build (" + e.name + ")";
    le.file = e.script->path;
    le.entity = e.id;
    le.tick = world_.script_tick();
    world_.script_log(std::move(le));
    return false;
  }
  if (owner(e.id)) return false;
  owner_[e.id] = slot_of(lang);
  return h->attach(e);
}

void ScriptHost::detach(std::uint64_t entity) {
  if (ScriptLanguageHost* h = owner(entity)) {
    h->detach(entity);
    owner_.erase(entity);
  }
}

bool ScriptHost::has(std::uint64_t entity) const {
  const ScriptLanguageHost* h = owner(entity);
  return h && h->has(entity);
}

bool ScriptHost::running(std::uint64_t entity) const {
  const ScriptLanguageHost* h = owner(entity);
  return h && h->running(entity);
}

bool ScriptHost::has_hook(std::uint64_t entity, const char* hook) {
  ScriptLanguageHost* h = owner(entity);
  return h && h->has_hook(entity, hook);
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
  for (auto& h : hosts_) if (h) h->start_pending();
}

void ScriptHost::trigger(bool enter, std::uint64_t trigger, std::uint64_t other) {
  if (ScriptLanguageHost* h = owner(trigger)) h->trigger(enter, trigger, other);
}

void ScriptHost::interact(std::uint64_t target, std::uint64_t rider) {
  if (ScriptLanguageHost* h = owner(target)) h->interact(target, rider);
}

void ScriptHost::ui_event(const std::string& widget, bool change, double value,
                          const std::string& text) {
  for (auto& h : hosts_) if (h) h->ui_event(widget, change, value, text);
}

void ScriptHost::hand_event(std::uint64_t target, const char* hook, const std::string& text,
                            double number) {
  if (ScriptLanguageHost* h = owner(target)) h->hand_event(target, hook, text, number);
}

void ScriptHost::broadcast(const char* hook, const std::string& text, double number) {
  for (auto& h : hosts_) if (h) h->broadcast(hook, text, number);
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
  for (auto& h : hosts_) {
    if (!h) continue;
    TS_PROFILE_SCOPE(tick_scope(h->language()));
    h->tick(dt);
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
  return true;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
