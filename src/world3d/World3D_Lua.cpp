// 3D methods on script entity handles: pos3 / set_pos3 / move3 / yaw /
// set_yaw / spin / scale3 / set_scale3. Added to the shared handle
// metatable when a 3D world builds its script sandbox.

#include "world3d/World3D.h"

#include "lauxlib.h"
#include "lua.h"

#include <cmath>

namespace ts {
namespace tombstone {
namespace world3d {

namespace {

constexpr const char* kEntityMeta = "TombStone.Entity";  // see ScriptApi.cpp

World3D& world(lua_State* L) {
  return *static_cast<World3D*>(lua_touserdata(L, lua_upvalueindex(1)));
}

Entity3D* self(lua_State* L) {
  const auto* id = static_cast<std::uint64_t*>(luaL_checkudata(L, 1, kEntityMeta));
  return world(L).live(*id);
}

float num(lua_State* L, int idx) {
  const lua_Number n = luaL_checknumber(L, idx);
  luaL_argcheck(L, std::isfinite(n) && std::fabs(n) < 1e7, idx, "coordinate out of range");
  return static_cast<float>(n);
}

int pos3(lua_State* L) {
  const Entity3D* e = self(L);
  if (!e) return 0;
  lua_pushnumber(L, e->transform.position.x);
  lua_pushnumber(L, e->transform.position.y);
  lua_pushnumber(L, e->transform.position.z);
  return 3;
}

int set_pos3(lua_State* L) {
  if (Entity3D* e = self(L)) e->transform.position = {num(L, 2), num(L, 3), num(L, 4)};
  return 0;
}

int move3(lua_State* L) {
  if (Entity3D* e = self(L)) e->transform.position += render3d::Vec3{num(L, 2), num(L, 3), num(L, 4)};
  return 0;
}

int yaw(lua_State* L) {
  const Entity3D* e = self(L);
  if (!e) return 0;
  lua_pushnumber(L, e->transform.rotation.to_euler_deg().y);
  return 1;
}

int set_yaw(lua_State* L) {
  if (Entity3D* e = self(L)) {
    render3d::Vec3 eu = e->transform.rotation.to_euler_deg();
    eu.y = num(L, 2);
    e->transform.rotation = render3d::Quat::from_euler_deg(eu);
  }
  return 0;
}

// spin(deg [, axis "x"|"y"|"z"]): turn about a local axis (default: the
// entity's local Z for flat things like sails is surprising, so Y).
int spin(lua_State* L) {
  if (Entity3D* e = self(L)) {
    const float deg = num(L, 2);
    const char* axis = luaL_optstring(L, 3, "y");
    render3d::Vec3 a{0, 1, 0};
    if (axis[0] == 'x') a = {1, 0, 0};
    else if (axis[0] == 'z') a = {0, 0, 1};
    e->transform.rotation = render3d::normalize(
        e->transform.rotation * render3d::Quat::axis_angle(a, render3d::radians(deg)));
  }
  return 0;
}

int scale3(lua_State* L) {
  const Entity3D* e = self(L);
  if (!e) return 0;
  lua_pushnumber(L, e->transform.scale.x);
  lua_pushnumber(L, e->transform.scale.y);
  lua_pushnumber(L, e->transform.scale.z);
  return 3;
}

int set_scale3(lua_State* L) {
  if (Entity3D* e = self(L)) e->transform.scale = {num(L, 2), num(L, 3), num(L, 4)};
  return 0;
}

}  // namespace

void World3D::script_open_extensions(lua_State* L, int /*sandbox*/) {
  if (luaL_getmetatable(L, kEntityMeta) != LUA_TTABLE) {
    lua_pop(L, 1);
    return;
  }
  lua_getfield(L, -1, "__index");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 2);
    return;
  }
  const luaL_Reg methods[] = {{"pos3", pos3},     {"set_pos3", set_pos3}, {"move3", move3},
                              {"yaw", yaw},       {"set_yaw", set_yaw},   {"spin", spin},
                              {"scale3", scale3}, {"set_scale3", set_scale3},
                              {nullptr, nullptr}};
  lua_pushlightuserdata(L, this);
  luaL_setfuncs(L, methods, 1);
  lua_pop(L, 2);
}

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
