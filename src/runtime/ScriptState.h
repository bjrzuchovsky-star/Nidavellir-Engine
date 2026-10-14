#pragma once

// Internal to Script.cpp / ScriptApi.cpp: the Lua side of ScriptHost.

#include "runtime/Script.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

struct ScriptInstance {
  std::uint64_t entity = 0;
  std::string path;
  std::vector<ScriptProp> overrides;  // the entity's Inspector values
  int env = LUA_NOREF;                // registry ref: the instance's globals
  bool loaded = false;                // top level ran
  bool started = false;               // on_start called
  bool enabled = false;               // false once it errored
  bool gone = false;                  // detached; swept when idle
};

struct ScriptTimer {
  std::uint64_t id = 0;
  std::uint64_t entity = 0;
  std::uint64_t due = 0;     // tick it fires on
  std::uint64_t period = 0;  // ticks between repeats (0 = once)
  int fn = LUA_NOREF;
};

struct ScriptUiHandler {
  std::uint64_t entity = 0;
  std::string widget;
  bool change = false;  // on_change (else on_click)
  int fn = LUA_NOREF;
};

struct ScriptHost::State {
  ScriptWorld* world = nullptr;  // null while describing a file
  ScriptLibrary* library = nullptr;
  std::string project_dir;
  lua_State* L = nullptr;
  bool broken = false;  // the VM itself failed; scripts are off

  std::size_t mem_used = 0;
  int depth = 0;                  // script calls on the C++ stack
  long long instructions = 0;     // spent by the outermost call
  ScriptInstance* current = nullptr;
  std::string last_error;         // describe(): first failure

  std::vector<std::unique_ptr<ScriptInstance>> instances;
  std::vector<ScriptTimer> timers;
  std::vector<ScriptUiHandler> ui_handlers;
  std::uint64_t next_timer = 1;
  std::uint64_t rng = 0x9E3779B97F4A7C15ull;  // math.random (replayable)

  int sandbox = LUA_NOREF;   // shared API + safe libraries
  int env_meta = LUA_NOREF;  // metatable of every instance env
  int handles = LUA_NOREF;   // id -> entity handle (weak)

  ~State();
  static State* from(lua_State* L);

  bool open();
  ScriptInstance* find(std::uint64_t entity);
  // Push the instance's hook; false (nothing pushed) when it has none or
  // cannot run now.
  bool push_hook(ScriptInstance& in, const char* hook);
  // Call the function under nargs args on the stack, blaming `in` (may be
  // null) for errors. Pops everything.
  bool call(ScriptInstance* in, int nargs);
  // Load + run the file's top level in the instance env.
  bool run_file(ScriptInstance& in, const std::string& text);
  void apply_props(ScriptInstance& in);
  void fail(ScriptInstance* in, const std::string& message);
  void drop_timers(std::uint64_t entity);
  void sweep();
  void log(LogLevel level, const std::string& text, const std::string& file,
           int line, std::uint64_t entity);
  std::string entity_name(std::uint64_t id) const;
};

// Lua API (ScriptApi.cpp): fill the sandbox table on top of the stack.
void open_script_api(lua_State* L);
// Entity handle for id (nil for 0). One handle per id while it is in use,
// so handles work as table keys.
void push_entity(lua_State* L, std::uint64_t id);
void push_script_value(lua_State* L, const ScriptValue& v);
// Booleans, numbers and strings; false for anything else.
bool to_script_value(lua_State* L, int idx, ScriptValue* out);
// "scripts/gate.lua:12: boom" -> file, line, "boom"; false if unlocated.
bool split_location(const std::string& msg, std::string* file, int* line,
                    std::string* rest);

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
