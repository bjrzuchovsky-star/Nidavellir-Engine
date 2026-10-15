// The plugin call bridge: plugins hand Lua tables to scripts (WorldExtensions
// ::lua). Graphs, C++ and C# reach the same tables through
// ScriptWorld::script_plugin_call, which keeps one small Lua state per world
// holding every plugin table and calls table.fn(args...) in it.

#include "runtime/Script.h"

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

namespace {

constexpr int kBridgeBudget = 2000000;  // instructions per call

void budget_hook(lua_State* L, lua_Debug*) { luaL_error(L, "plugin call ran too long (instruction budget)"); }

void push_value(lua_State* L, const ScriptValue& v) {
  switch (v.type) {
    case ScriptValue::Type::Bool: lua_pushboolean(L, v.flag ? 1 : 0); break;
    case ScriptValue::Type::Number: lua_pushnumber(L, v.number); break;
    case ScriptValue::Type::Text: lua_pushlstring(L, v.text.data(), v.text.size()); break;
  }
}

ScriptValue read_value(lua_State* L, int idx) {
  switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: return ScriptValue::of_bool(lua_toboolean(L, idx) != 0);
    case LUA_TNUMBER: return ScriptValue::of_number(lua_tonumber(L, idx));
    case LUA_TSTRING: {
      std::size_t n = 0;
      const char* s = lua_tolstring(L, idx, &n);
      return ScriptValue::of_text(std::string(s, n));
    }
    case LUA_TNIL:
    case LUA_TNONE: return ScriptValue::of_bool(false);
    default: return ScriptValue::of_text(luaL_typename(L, idx));
  }
}

}  // namespace

struct ScriptWorld::PluginBridge {
  lua_State* L = nullptr;
  int sandbox = LUA_NOREF;
  ~PluginBridge() {
    if (L) lua_close(L);
  }
};

void ScriptWorld::PluginBridgeDeleter::operator()(PluginBridge* b) const { delete b; }

bool ScriptWorld::script_plugin_call(const std::string& table, const std::string& fn,
                                     const std::vector<ScriptValue>& args, ScriptValue* result,
                                     std::string* error) {
  std::string scratch;
  std::string& err = error ? *error : scratch;
  if (!plugin_bridge_) {
    plugin_bridge_.reset(new PluginBridge());
    lua_State* L = luaL_newstate();
    if (!L) {
      err = "plugin bridge: out of memory";
      return false;
    }
    plugin_bridge_->L = L;
    // Same safe libraries the script sandbox offers.
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_settop(L, 0);
    lua_newtable(L);
    script_open_extensions(L, lua_absindex(L, -1));
    plugin_bridge_->sandbox = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  lua_State* L = plugin_bridge_->L;
  if (!L) {
    err = "plugin bridge unavailable";
    return false;
  }
  lua_settop(L, 0);
  lua_rawgeti(L, LUA_REGISTRYINDEX, plugin_bridge_->sandbox);
  lua_getfield(L, 1, table.c_str());
  if (!lua_istable(L, -1)) {
    lua_settop(L, 0);
    err = "no plugin table '" + table + "' (is the plugin loaded?)";
    return false;
  }
  lua_getfield(L, -1, fn.c_str());
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, 0);
    err = "plugin table '" + table + "' has no function '" + fn + "'";
    return false;
  }
  for (const ScriptValue& a : args) push_value(L, a);
  lua_sethook(L, &budget_hook, LUA_MASKCOUNT, kBridgeBudget);
  const int rc = lua_pcall(L, static_cast<int>(args.size()), 1, 0);
  lua_sethook(L, nullptr, 0, 0);
  if (rc != LUA_OK) {
    const char* m = lua_tostring(L, -1);
    err = table + "." + fn + ": " + (m ? m : "error");
    lua_settop(L, 0);
    return false;
  }
  if (result) *result = read_value(L, -1);
  lua_settop(L, 0);
  return true;
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
