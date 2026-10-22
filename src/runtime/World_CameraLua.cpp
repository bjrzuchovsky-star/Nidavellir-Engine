// The Lua `camera` table of the 2D World (docs/camera.md). Graphs, C++ and
// C# reach the same functions through the plugin call bridge
// (script_plugin_call("camera", ...)).
//
//   camera.active() -> id (0 = no Camera2D: the built-in follow view)
//   camera.set_active(e | id | name) -> ok      camera.reset()
//   camera.set_priority(e, p) -> ok              camera.count() -> cameras
//   camera.set_zoom(e, zoom) -> ok

#include "runtime/World.h"

#include "lauxlib.h"
#include "lua.h"

#include <cstdint>
#include <string>

namespace ts {
namespace tombstone {
namespace runtime {

namespace {

constexpr const char* kEntityMeta = "TombStone.Entity";  // see ScriptApi.cpp

World& W(lua_State* L) { return *static_cast<World*>(lua_touserdata(L, lua_upvalueindex(1))); }

std::uint64_t cam_arg(lua_State* L, int idx) {
  if (const auto* u = static_cast<std::uint64_t*>(luaL_testudata(L, idx, kEntityMeta))) return *u;
  if (lua_type(L, idx) == LUA_TSTRING) {
    const std::string name = lua_tostring(L, idx);
    for (const Actor& a : W(L).actors())
      if (!a.dead && a.data.camera && a.data.name == name) return a.data.id;
    return 0;
  }
  const lua_Number n = luaL_checknumber(L, idx);
  return n > 0 ? static_cast<std::uint64_t>(n) : 0;
}

int l_active(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(W(L).camera_entity()));
  return 1;
}
int l_set_active(lua_State* L) {
  const std::uint64_t id = cam_arg(L, 1);
  lua_pushboolean(L, id != 0 && W(L).set_active_camera(id));
  return 1;
}
int l_reset(lua_State* L) {
  W(L).set_active_camera(0);
  return 0;
}
int l_set_priority(lua_State* L) {
  lua_pushboolean(L, W(L).set_camera_priority(cam_arg(L, 1), static_cast<int>(luaL_checknumber(L, 2))));
  return 1;
}
int l_set_zoom(lua_State* L) {
  lua_pushboolean(L, W(L).set_camera_zoom(cam_arg(L, 1), static_cast<float>(luaL_checknumber(L, 2))));
  return 1;
}
int l_count(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(W(L).camera_count()));
  return 1;
}

}  // namespace

void World::open_camera_lua(lua_State* L, int sandbox) {
  const luaL_Reg fns[] = {{"active", l_active},     {"set_active", l_set_active},
                          {"reset", l_reset},       {"set_priority", l_set_priority},
                          {"set_zoom", l_set_zoom}, {"count", l_count},
                          {nullptr, nullptr}};
  lua_newtable(L);
  lua_pushlightuserdata(L, this);
  luaL_setfuncs(L, fns, 1);
  lua_setfield(L, sandbox, "camera");
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
