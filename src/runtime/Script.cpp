// Script library, the sandboxed Lua state and the hook / timer / reload
// machinery. The functions scripts call live in ScriptApi.cpp.

#include "runtime/ScriptState.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace ts {
namespace tombstone {
namespace runtime {

namespace fs = std::filesystem;

bool valid_script_path(const std::string& rel) {
  if (rel.size() < 5 || rel.size() > 260 ||
      rel.compare(rel.size() - 4, 4, ".lua") != 0) {
    return false;
  }
  if (rel.find('\\') != std::string::npos || rel.find(':') != std::string::npos) {
    return false;
  }
  std::size_t start = 0;
  while (start <= rel.size()) {
    std::size_t end = rel.find('/', start);
    if (end == std::string::npos) {
      end = rel.size();
    }
    const std::string_view part(rel.data() + start, end - start);
    if (part.empty() || part == "." || part == "..") {
      return false;
    }
    start = end + 1;
  }
  return true;
}

bool split_location(const std::string& msg, std::string* file, int* line,
                    std::string* rest) {
  // <something>.lua:<digits>: <text>
  const std::size_t dot = msg.find(".lua:");
  if (dot == std::string::npos) {
    return false;
  }
  std::size_t p = dot + 5;
  int n = 0;
  std::size_t digits = 0;
  while (p < msg.size() && msg[p] >= '0' && msg[p] <= '9' && digits < 9) {
    n = n * 10 + (msg[p] - '0');
    ++p;
    ++digits;
  }
  if (digits == 0 || p >= msg.size() || msg[p] != ':') {
    return false;
  }
  if (file) *file = msg.substr(0, dot + 4);
  if (line) *line = n;
  if (rest) {
    ++p;
    while (p < msg.size() && msg[p] == ' ') ++p;
    *rest = msg.substr(p);
  }
  return true;
}

// --- ScriptLibrary ----------------------------------------------------------

namespace {

std::int64_t stamp_of(const fs::path& p) {
  std::error_code ec;
  const auto t = fs::last_write_time(p, ec);
  if (ec) {
    return -1;
  }
  const auto size = fs::file_size(p, ec);
  const auto ticks = static_cast<std::int64_t>(t.time_since_epoch().count());
  return ticks ^ (ec ? 0 : static_cast<std::int64_t>(size) * 1000003);
}

}  // namespace

ScriptLibrary::Entry* ScriptLibrary::slot(const std::string& rel) {
  for (Entry& e : entries_) {
    if (e.path == rel) {
      return &e;
    }
  }
  return nullptr;
}

void ScriptLibrary::read(const std::string& project_dir, Entry* e) {
  const std::string old_text = e->text;
  const bool old_ok = e->ok;
  const std::string old_error = e->error;
  e->text.clear();
  e->ok = false;
  e->error.clear();
  const fs::path p = fs::path(project_dir) / fs::path(e->path);
  e->stamp = stamp_of(p);
  if (!valid_script_path(e->path)) {
    e->error = "Not a script path (want scripts/<name>.lua)";
  } else if (e->stamp == -1) {
    e->error = "Script not found";
  } else {
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    std::ifstream in(p, std::ios::binary);
    if (ec || !in) {
      e->error = "Could not read the script";
    } else if (size > kMaxBytes) {
      e->error = "Script is larger than 1 MB";
    } else {
      e->text.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
      e->ok = true;
    }
  }
  if (e->text != old_text || e->ok != old_ok || e->error != old_error) {
    ++e->version;
    e->dirty = true;
  }
}

const ScriptLibrary::Entry* ScriptLibrary::get(const std::string& project_dir,
                                               const std::string& rel) {
  Entry* e = slot(rel);
  if (!e) {
    entries_.push_back(Entry{});
    e = &entries_.back();
    e->path = rel;
    read(project_dir, e);
    e->dirty = false;
    return e;
  }
  if (!e->memory &&
      stamp_of(fs::path(project_dir) / fs::path(rel)) != e->stamp) {
    read(project_dir, e);
  }
  return e;
}

void ScriptLibrary::put(const std::string& rel, std::string text) {
  Entry* e = slot(rel);
  if (!e) {
    entries_.push_back(Entry{});
    e = &entries_.back();
    e->path = rel;
  }
  if (!e->memory || !e->ok || e->text != text) {
    ++e->version;
    e->dirty = true;
  }
  e->memory = true;
  e->ok = true;
  e->error.clear();
  e->text = std::move(text);
}

void ScriptLibrary::forget(const std::string& rel) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [&](const Entry& e) { return e.path == rel; }),
                 entries_.end());
}

std::vector<std::string> ScriptLibrary::refresh(const std::string& project_dir) {
  std::vector<std::string> changed;
  for (Entry& e : entries_) {
    if (!e.memory &&
        stamp_of(fs::path(project_dir) / fs::path(e.path)) != e.stamp) {
      read(project_dir, &e);
    }
    if (e.dirty) {
      e.dirty = false;
      changed.push_back(e.path);
    }
  }
  return changed;
}

// --- The Lua state ----------------------------------------------------------

namespace {

constexpr int kHookEvery = 1000;  // instructions between budget checks

void* script_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
  auto* s = static_cast<ScriptHost::State*>(ud);
  if (!ptr) {
    osize = 0;  // osize carries the object type for new blocks
  }
  if (nsize == 0) {
    std::free(ptr);
    s->mem_used -= std::min(osize, s->mem_used);
    return nullptr;
  }
  // The cap only bites while a script runs, so host bookkeeping never
  // fails half-way.
  if (s->depth > 0 && nsize > osize &&
      s->mem_used - osize + nsize > ScriptHost::kMemoryLimit) {
    return nullptr;
  }
  void* p = std::realloc(ptr, nsize);
  if (p) {
    s->mem_used = s->mem_used - osize + nsize;
  }
  return p;
}

int script_panic(lua_State* L) {
  const char* msg = lua_tostring(L, -1);
  throw std::runtime_error(msg ? msg : "unprotected Lua error");
}

void count_hook(lua_State* L, lua_Debug*) {
  auto* s = ScriptHost::State::from(L);
  s->instructions += kHookEvery;
  if (s->instructions <= ScriptHost::kInstructionBudget) {
    return;
  }
  lua_Debug ar;
  if (lua_getstack(L, 0, &ar) && lua_getinfo(L, "Sl", &ar) &&
      ar.currentline > 0) {
    lua_pushfstring(L, "%s:%d: ", ar.short_src, ar.currentline);
  } else {
    lua_pushliteral(L, "");
  }
  lua_pushfstring(L,
                  "ran too long (over %d instructions in one call; endless "
                  "loop?)",
                  static_cast<int>(ScriptHost::kInstructionBudget));
  lua_concat(L, 2);
  lua_error(L);
}

// Error handler: a string message with "file:line:" in front when Lua
// itself did not put one there.
int message_handler(lua_State* L) {
  if (lua_type(L, 1) != LUA_TSTRING) {
    if (!luaL_callmeta(L, 1, "__tostring") || lua_type(L, -1) != LUA_TSTRING) {
      lua_pushfstring(L, "error object is a %s value", luaL_typename(L, 1));
    }
  } else {
    lua_pushvalue(L, 1);
  }
  const std::string msg = lua_tostring(L, -1);
  if (split_location(msg, nullptr, nullptr, nullptr)) {
    return 1;
  }
  lua_Debug ar;
  for (int level = 1; lua_getstack(L, level, &ar); ++level) {
    if (lua_getinfo(L, "Sl", &ar) && ar.currentline > 0 && ar.source &&
        ar.source[0] == '@') {
      lua_pushfstring(L, "%s:%d: %s", ar.source + 1, ar.currentline,
                      msg.c_str());
      return 1;
    }
  }
  return 1;
}

}  // namespace

ScriptHost::State::~State() {
  if (L) {
    lua_close(L);
  }
}

ScriptHost::State* ScriptHost::State::from(lua_State* L) {
  State* s = nullptr;
  std::memcpy(&s, lua_getextraspace(L), sizeof s);
  return s;
}

bool ScriptHost::State::open() {
  L = lua_newstate(&script_alloc, this);
  if (!L) {
    return false;
  }
  State* self = this;
  std::memcpy(lua_getextraspace(L), &self, sizeof self);
  lua_atpanic(L, &script_panic);
  // Only the pure libraries; os / io / package / debug are not even built.
  const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},
                           {LUA_TABLIBNAME, luaopen_table},
                           {LUA_STRLIBNAME, luaopen_string},
                           {LUA_MATHLIBNAME, luaopen_math},
                           {LUA_UTF8LIBNAME, luaopen_utf8},
                           {LUA_COLIBNAME, luaopen_coroutine}};
  for (const luaL_Reg& lib : libs) {
    luaL_requiref(L, lib.name, lib.func, 1);
    lua_pop(L, 1);
  }
  lua_sethook(L, &count_hook, LUA_MASKCOUNT, kHookEvery);

  // string.dump hands out bytecode; strings share one locked metatable.
  lua_getglobal(L, LUA_STRLIBNAME);
  lua_pushnil(L);
  lua_setfield(L, -2, "dump");
  lua_pop(L, 1);
  lua_pushliteral(L, "");
  if (lua_getmetatable(L, -1)) {
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  // The sandbox: what every instance env falls back to. No load / dofile /
  // loadfile / require / collectgarbage / print (log replaces it).
  lua_newtable(L);
  const char* const keep[] = {
      "assert", "error",    "ipairs",       "next",         "pairs",
      "pcall",  "rawequal", "rawget",       "rawlen",       "rawset",
      "select", "tonumber", "tostring",     "type",         "xpcall",
      "setmetatable", "getmetatable", "_VERSION", LUA_TABLIBNAME,
      LUA_STRLIBNAME, LUA_MATHLIBNAME, LUA_UTF8LIBNAME, LUA_COLIBNAME};
  for (const char* name : keep) {
    lua_getglobal(L, name);
    lua_setfield(L, -2, name);
  }
  open_script_api(L);
  if (world) {
    world->script_open_extensions(L, lua_gettop(L));
  }
  sandbox = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_newtable(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, sandbox);
  lua_setfield(L, -2, "__index");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "__metatable");
  env_meta = luaL_ref(L, LUA_REGISTRYINDEX);

  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, -2);
  handles = luaL_ref(L, LUA_REGISTRYINDEX);
  return true;
}

ScriptInstance* ScriptHost::State::find(std::uint64_t entity) {
  for (auto& in : instances) {
    if (in->entity == entity && !in->gone) {
      return in.get();
    }
  }
  return nullptr;
}

std::string ScriptHost::State::entity_name(std::uint64_t id) const {
  const Entity2D* e = world ? world->script_entity(id) : nullptr;
  return e ? e->name : "#" + std::to_string(id);
}

void ScriptHost::State::log(LogLevel level, const std::string& text,
                            const std::string& file, int line,
                            std::uint64_t entity) {
  if (!world) {
    return;
  }
  LogEntry e;
  e.level = level;
  e.channel = "script";
  e.text = text;
  e.file = file;
  e.line = line;
  e.entity = entity;
  e.tick = world->script_tick();
  world->script_log(std::move(e));
}

void ScriptHost::State::fail(ScriptInstance* in, const std::string& message) {
  std::string file;
  int line = 0;
  std::string rest = message;
  if (!split_location(message, &file, &line, &rest) && in) {
    file = in->path;
  }
  if (last_error.empty()) {
    last_error = message;
  }
  if (!in) {
    log(LogLevel::Error, rest, file, line, 0);
    return;
  }
  const bool was_running = in->enabled;
  in->enabled = false;
  drop_timers(in->entity);
  if (was_running || !in->loaded) {
    log(LogLevel::Error,
        rest + " -- " + entity_name(in->entity) +
            "'s script is stopped; the rest of the world rides on",
        file, line, in->entity);
  } else {
    log(LogLevel::Error, rest, file, line, in->entity);
  }
}

void ScriptHost::State::drop_timers(std::uint64_t entity) {
  auto it = std::remove_if(timers.begin(), timers.end(), [&](const ScriptTimer& t) {
    if (t.entity != entity) {
      return false;
    }
    luaL_unref(L, LUA_REGISTRYINDEX, t.fn);
    return true;
  });
  timers.erase(it, timers.end());
  auto ui = std::remove_if(ui_handlers.begin(), ui_handlers.end(), [&](const ScriptUiHandler& h) {
    if (h.entity != entity) {
      return false;
    }
    luaL_unref(L, LUA_REGISTRYINDEX, h.fn);
    return true;
  });
  ui_handlers.erase(ui, ui_handlers.end());
}

bool ScriptHost::State::push_hook(ScriptInstance& in, const char* hook) {
  if (in.gone || !in.enabled || !in.loaded ||
      (world && !world->script_entity(in.entity))) {
    return false;
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, in.env);
  lua_pushstring(L, hook);
  lua_rawget(L, -2);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 2);
    return false;
  }
  lua_remove(L, -2);
  return true;
}

bool ScriptHost::State::call(ScriptInstance* in, int nargs) {
  const int base = lua_gettop(L) - nargs;
  lua_pushcfunction(L, &message_handler);
  lua_insert(L, base);
  ScriptInstance* saved = current;
  current = in;
  if (depth == 0) {
    instructions = 0;
  }
  ++depth;
  const int status = lua_pcall(L, nargs, 0, base);
  --depth;
  current = saved;
  if (status != LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    const std::string text = msg ? msg : "unknown error";
    lua_pop(L, 2);
    fail(in, status == LUA_ERRMEM ? text + " (scripts share a " +
                                        std::to_string(ScriptHost::kMemoryLimit >> 20) +
                                        " MB budget)"
                                  : text);
    return false;
  }
  lua_pop(L, 1);
  return true;
}

bool ScriptHost::State::run_file(ScriptInstance& in, const std::string& text) {
  const std::string chunk = "@" + in.path;
  // Text only: precompiled bytecode can break out of any sandbox.
  if (luaL_loadbufferx(L, text.data(), text.size(), chunk.c_str(), "t") !=
      LUA_OK) {
    const char* msg = lua_tostring(L, -1);
    const std::string err = msg ? msg : "does not compile";
    lua_pop(L, 1);
    in.loaded = false;
    fail(&in, err);
    return false;
  }
  lua_rawgeti(L, LUA_REGISTRYINDEX, in.env);
  lua_setupvalue(L, -2, 1);  // the chunk's _ENV
  in.loaded = true;
  in.enabled = true;
  if (!call(&in, 0)) {
    return false;
  }
  apply_props(in);
  return true;
}

void ScriptHost::State::apply_props(ScriptInstance& in) {
  lua_rawgeti(L, LUA_REGISTRYINDEX, in.env);
  lua_pushliteral(L, "props");
  lua_rawget(L, -2);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushliteral(L, "props");
    lua_pushvalue(L, -2);
    lua_rawset(L, -4);
  }
  for (const ScriptProp& p : in.overrides) {
    push_script_value(L, p.value);
    lua_setfield(L, -2, p.name.c_str());
  }
  lua_pop(L, 2);
}

void ScriptHost::State::sweep() {
  if (depth > 0) {
    return;
  }
  auto it = std::remove_if(instances.begin(), instances.end(), [&](const auto& in) {
    if (!in->gone) {
      return false;
    }
    luaL_unref(L, LUA_REGISTRYINDEX, in->env);
    return true;
  });
  instances.erase(it, instances.end());
}

// --- ScriptHost -------------------------------------------------------------

namespace {

// Run `body` unless the VM is down; an unprotected Lua failure (it should
// never happen: every script call is protected) turns scripting off for
// this ride instead of taking the process with it.
template <typename F>
void guarded(ScriptHost::State& s, F&& body) {
  if (s.broken || !s.L) {
    return;
  }
  try {
    body();
  } catch (const std::exception& ex) {
    s.broken = true;
    s.depth = 0;
    s.current = nullptr;
    s.log(LogLevel::Error,
          std::string("The script engine fell over (") + ex.what() +
              "); scripts are off for the rest of this ride",
          std::string(), 0, 0);
  }
}

}  // namespace

ScriptHost::ScriptHost(ScriptWorld& world, ScriptLibrary& library,
                       std::string project_dir)
    : s_(std::make_unique<State>()) {
  s_->world = &world;
  s_->library = &library;
  s_->project_dir = std::move(project_dir);
  if (!s_->open()) {
    s_->broken = true;
    s_->log(LogLevel::Error, "Could not start the script engine", {}, 0, 0);
  }
}

ScriptHost::~ScriptHost() = default;

bool ScriptHost::attach(const Entity2D& e) {
  if (!e.script) {
    return false;
  }
  bool ok = false;
  guarded(*s_, [&] {
    State& s = *s_;
    if (s.find(e.id)) {
      return;
    }
    auto owned = std::make_unique<ScriptInstance>();
    ScriptInstance& in = *owned;
    in.entity = e.id;
    in.path = e.script->path;
    in.overrides = e.script->props;
    lua_newtable(s.L);
    lua_rawgeti(s.L, LUA_REGISTRYINDEX, s.env_meta);
    lua_setmetatable(s.L, -2);
    push_entity(s.L, e.id);
    lua_setfield(s.L, -2, "self");
    in.env = luaL_ref(s.L, LUA_REGISTRYINDEX);
    s.instances.push_back(std::move(owned));
    if (in.path.empty()) {
      s.log(LogLevel::Warn, e.name + " has a script component but no file",
            {}, 0, e.id);
      return;
    }
    const ScriptLibrary::Entry* src = s.library->get(s.project_dir, in.path);
    if (!src || !src->ok) {
      in.loaded = false;
      s.log(LogLevel::Error,
            (src ? src->error : std::string("Script not found")) + " (" +
                e.name + ")",
            in.path, 0, e.id);
      return;
    }
    ok = s.run_file(in, src->text);
  });
  return ok;
}

void ScriptHost::detach(std::uint64_t entity) {
  guarded(*s_, [&] {
    if (ScriptInstance* in = s_->find(entity)) {
      in->gone = true;
      in->enabled = false;
      s_->drop_timers(entity);
      s_->sweep();
    }
  });
}

bool ScriptHost::has(std::uint64_t entity) const {
  return s_->find(entity) != nullptr;
}

bool ScriptHost::running(std::uint64_t entity) const {
  const ScriptInstance* in = s_->find(entity);
  return in && in->loaded && in->enabled && !s_->broken;
}

bool ScriptHost::has_hook(std::uint64_t entity, const char* hook) {
  bool yes = false;
  guarded(*s_, [&] {
    ScriptInstance* in = s_->find(entity);
    if (in && s_->push_hook(*in, hook)) {
      lua_pop(s_->L, 1);
      yes = true;
    }
  });
  return yes;
}

std::size_t ScriptHost::instance_count() const { return s_->instances.size(); }
std::size_t ScriptHost::timer_count() const { return s_->timers.size(); }
std::size_t ScriptHost::memory_used() const { return s_->mem_used; }

void ScriptHost::start_pending() {
  guarded(*s_, [&] {
    State& s = *s_;
    // Index loop: on_start may spawn (append) more instances.
    for (std::size_t i = 0; i < s.instances.size(); ++i) {
      ScriptInstance* in = s.instances[i].get();
      if (in->started || in->gone || !in->loaded || !in->enabled) {
        continue;
      }
      in->started = true;
      if (s.push_hook(*in, "on_start")) {
        s.call(in, 0);
      }
    }
    s.sweep();
  });
}

void ScriptHost::trigger(bool enter, std::uint64_t trigger,
                         std::uint64_t other) {
  guarded(*s_, [&] {
    State& s = *s_;
    ScriptInstance* in = s.find(trigger);
    if (in && in->started &&
        s.push_hook(*in, enter ? "on_trigger_enter" : "on_trigger_exit")) {
      push_entity(s.L, other);
      s.call(in, 1);
    }
    s.sweep();
  });
}

void ScriptHost::interact(std::uint64_t target, std::uint64_t rider) {
  guarded(*s_, [&] {
    State& s = *s_;
    ScriptInstance* in = s.find(target);
    if (in && in->started && s.push_hook(*in, "on_interact")) {
      push_entity(s.L, rider);
      s.call(in, 1);
    }
    s.sweep();
  });
}

void ScriptHost::hand_event(std::uint64_t target, const char* hook, const std::string& text,
                            double number) {
  guarded(*s_, [&] {
    State& s = *s_;
    ScriptInstance* in = s.find(target);
    if (in && in->started && s.push_hook(*in, hook)) {
      lua_pushlstring(s.L, text.data(), text.size());
      lua_pushnumber(s.L, number);
      s.call(in, 2);
    }
    s.sweep();
  });
}

void ScriptHost::ui_event(const std::string& widget, bool change, double value,
                          const std::string& text) {
  guarded(*s_, [&] {
    State& s = *s_;
    // Copy first: a handler may add or drop handlers.
    const std::vector<ScriptUiHandler> hs = s.ui_handlers;
    for (const ScriptUiHandler& h : hs) {
      if (h.change != change || h.widget != widget) continue;
      ScriptInstance* in = s.find(h.entity);
      if (!in || !in->enabled || !s.world->script_entity(h.entity)) continue;
      lua_rawgeti(s.L, LUA_REGISTRYINDEX, h.fn);
      lua_pushlstring(s.L, widget.data(), widget.size());
      lua_pushnumber(s.L, value);
      lua_pushlstring(s.L, text.data(), text.size());
      s.call(in, 3);
    }
    s.sweep();
  });
}

std::size_t ScriptHost::ui_handler_count() const { return s_->ui_handlers.size(); }

void ScriptHost::run_timers(std::uint64_t tick) {
  guarded(*s_, [&] {
    State& s = *s_;
    for (;;) {
      // Earliest due first; same tick fires in the order they were set.
      auto best = s.timers.end();
      for (auto it = s.timers.begin(); it != s.timers.end(); ++it) {
        if (it->due <= tick &&
            (best == s.timers.end() || it->due < best->due ||
             (it->due == best->due && it->id < best->id))) {
          best = it;
        }
      }
      if (best == s.timers.end()) {
        break;
      }
      const ScriptTimer t = *best;
      if (t.period > 0) {
        best->due += t.period;
      } else {
        s.timers.erase(best);
      }
      ScriptInstance* in = s.find(t.entity);
      lua_rawgeti(s.L, LUA_REGISTRYINDEX, t.fn);
      if (t.period == 0) {
        luaL_unref(s.L, LUA_REGISTRYINDEX, t.fn);
      }
      if (!in || !in->enabled || !s.world->script_entity(t.entity)) {
        lua_pop(s.L, 1);
        continue;
      }
      s.call(in, 0);
    }
    s.sweep();
  });
}

void ScriptHost::tick(double dt) {
  guarded(*s_, [&] {
    State& s = *s_;
    const std::size_t n = s.instances.size();  // spawned this tick wait
    for (std::size_t i = 0; i < n && i < s.instances.size(); ++i) {
      ScriptInstance* in = s.instances[i].get();
      if (in->started && s.push_hook(*in, "on_tick")) {
        lua_pushnumber(s.L, dt);
        s.call(in, 1);
      }
    }
    s.sweep();
  });
}

bool ScriptHost::reload(const std::string& rel) {
  bool ok = false;
  guarded(*s_, [&] {
    State& s = *s_;
    std::vector<ScriptInstance*> users;
    for (auto& in : s.instances) {
      if (!in->gone && in->path == rel) {
        users.push_back(in.get());
      }
    }
    if (users.empty()) {
      return;
    }
    const ScriptLibrary::Entry* src = s.library->get(s.project_dir, rel);
    if (!src || !src->ok) {
      s.log(LogLevel::Error,
            (src ? src->error : std::string("Script not found")) +
                "; kept the version already riding",
            rel, 0, 0);
      return;
    }
    // Compile once up front: a broken save keeps the old code running.
    const std::string chunk = "@" + rel;
    if (luaL_loadbufferx(s.L, src->text.data(), src->text.size(), chunk.c_str(),
                         "t") != LUA_OK) {
      const char* msg = lua_tostring(s.L, -1);
      std::string file = rel;
      int line = 0;
      std::string rest = msg ? msg : "does not compile";
      split_location(rest, &file, &line, &rest);
      lua_pop(s.L, 1);
      s.log(LogLevel::Error,
            rest + " -- reload skipped, the previous version keeps riding",
            file, line, 0);
      return;
    }
    lua_pop(s.L, 1);
    int good = 0;
    for (ScriptInstance* in : users) {
      if (!s.run_file(*in, src->text)) {
        continue;
      }
      ++good;
      if (in->started && s.push_hook(*in, "on_reload")) {
        s.call(in, 0);
      }
    }
    s.log(LogLevel::Info,
          "Reloaded " + rel + " (" + std::to_string(good) + " of " +
              std::to_string(users.size()) + " running)",
          rel, 0, 0);
    ok = good == static_cast<int>(users.size());
    s.sweep();
  });
  return ok;
}

int ScriptHost::reload_changed() {
  int n = 0;
  for (const std::string& rel : s_->library->refresh(s_->project_dir)) {
    const bool used = std::any_of(
        s_->instances.begin(), s_->instances.end(),
        [&](const auto& in) { return !in->gone && in->path == rel; });
    if (used) {
      reload(rel);
      ++n;
    }
  }
  return n;
}

bool ScriptHost::describe(const std::string& source, const std::string& rel,
                          std::vector<ScriptProp>* defaults,
                          std::string* error) {
  if (defaults) defaults->clear();
  State s;
  ScriptLibrary none;
  s.library = &none;
  bool ok = false;
  if (!s.open()) {
    s.last_error = "Could not start the script engine";
  }
  guarded(s, [&] {
    ScriptInstance in;
    in.path = rel;
    lua_newtable(s.L);
    lua_rawgeti(s.L, LUA_REGISTRYINDEX, s.env_meta);
    lua_setmetatable(s.L, -2);
    in.env = luaL_ref(s.L, LUA_REGISTRYINDEX);
    if (!s.run_file(in, source)) {
      return;
    }
    lua_rawgeti(s.L, LUA_REGISTRYINDEX, in.env);
    lua_getfield(s.L, -1, "props");
    lua_pushnil(s.L);
    while (lua_next(s.L, -2) != 0) {
      ScriptValue v;
      if (lua_type(s.L, -2) == LUA_TSTRING && to_script_value(s.L, -1, &v) &&
          defaults) {
        defaults->push_back(ScriptProp{lua_tostring(s.L, -2), v});
      }
      lua_pop(s.L, 1);
    }
    lua_pop(s.L, 2);
    ok = true;
  });
  if (s.broken && s.last_error.empty()) {
    s.last_error = "The script engine fell over";
  }
  if (defaults) {
    std::sort(defaults->begin(), defaults->end(),
              [](const ScriptProp& a, const ScriptProp& b) { return a.name < b.name; });
  }
  if (!ok && error) *error = s.last_error;
  return ok;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
