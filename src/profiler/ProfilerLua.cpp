#include "profiler/ProfilerLua.h"

#include "profiler/Console.h"
#include "profiler/Profiler.h"

#include "lauxlib.h"
#include "lua.h"

#include <mutex>
#include <set>
#include <string>

namespace ts {
namespace tombstone {
namespace profiler {

namespace {

// Scope names must outlive the scope; Lua strings may not. Interned once.
const char* intern(const std::string& s) {
  static std::mutex m;
  static std::set<std::string> pool;
  std::lock_guard<std::mutex> lock(m);
  if (pool.size() > 4096) return "lua.scope";
  return pool.insert("lua." + s).first->c_str();
}

thread_local int g_lua_depth = 0;

int p_begin(lua_State* L) {
  const char* n = luaL_checkstring(L, 1);
  if (Profiler::get().enabled() && g_lua_depth < 64) {
    Profiler::get().begin(intern(n));
    ++g_lua_depth;
  }
  return 0;
}
int p_finish(lua_State* /*L*/) {
  if (g_lua_depth > 0) {
    --g_lua_depth;
    Profiler::get().end();
  }
  return 0;
}
int p_counter(lua_State* L) {
  Profiler::get().counter("script." + std::string(luaL_checkstring(L, 1)), luaL_checknumber(L, 2));
  return 0;
}
int p_capture(lua_State* L) {
  std::string err;
  const std::string path = Profiler::get().capture(&err, luaL_optstring(L, 1, ""));
  if (path.empty()) {
    lua_pushnil(L);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  lua_pushstring(L, path.c_str());
  return 1;
}
int p_fps(lua_State* L) {
  lua_pushnumber(L, Profiler::get().fps());
  return 1;
}
int p_frame_ms(lua_State* L) {
  lua_pushnumber(L, Profiler::get().avg_frame_ms());
  return 1;
}
int p_enabled(lua_State* L) {
  lua_pushboolean(L, Profiler::get().enabled());
  return 1;
}
int p_enable(lua_State* L) {
  Profiler::get().set_enabled(lua_isnoneornil(L, 1) ? true : lua_toboolean(L, 1) != 0);
  return 0;
}
int p_scopes(lua_State* L) {
  const int n = static_cast<int>(luaL_optinteger(L, 1, 8));
  const auto st = Profiler::get().zone_stats(60);
  lua_newtable(L);
  for (int i = 0; i < n && i < static_cast<int>(st.size()); ++i) {
    lua_newtable(L);
    lua_pushstring(L, st[i].name.c_str());
    lua_setfield(L, -2, "name");
    lua_pushnumber(L, st[i].avg_ms);
    lua_setfield(L, -2, "avg");
    lua_pushnumber(L, st[i].max_ms);
    lua_setfield(L, -2, "max");
    lua_pushinteger(L, st[i].calls);
    lua_setfield(L, -2, "calls");
    lua_rawseti(L, -2, i + 1);
  }
  return 1;
}
int c_run(lua_State* L) {
  const std::string out = Console::shared().run(luaL_checkstring(L, 1));
  lua_pushstring(L, out.c_str());
  return 1;
}

}  // namespace

void push_profiler_lua(lua_State* L) {
  const luaL_Reg fns[] = {{"begin", &p_begin},     {"finish", &p_finish}, {"counter", &p_counter},
                          {"capture", &p_capture}, {"fps", &p_fps},       {"frame_ms", &p_frame_ms},
                          {"enabled", &p_enabled}, {"enable", &p_enable}, {"scopes", &p_scopes},
                          {nullptr, nullptr}};
  luaL_newlib(L, fns);
}

void push_console_lua(lua_State* L) {
  const luaL_Reg fns[] = {{"run", &c_run}, {nullptr, nullptr}};
  luaL_newlib(L, fns);
}

}  // namespace profiler
}  // namespace tombstone
}  // namespace ts
