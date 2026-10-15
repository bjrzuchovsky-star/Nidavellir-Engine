// The Lua `physics` table of the 2D World (Box2D bodies). Graphs, C++ and
// C# reach the same functions through the plugin call bridge
// (script_plugin_call("physics", ...)). Entities are handles or ids; units
// px, px / s, degrees (clockwise on screen).
//
//   physics.apply_force(e, fx, fy)      physics.apply_impulse(e, jx, jy)
//   physics.apply_torque(e, t)          physics.set_velocity(e, vx, vy)
//   physics.velocity(e) -> vx, vy       physics.speed(e) -> px / s
//   physics.raycast(x, y, dx, dy, dist) -> hit entity (or false), distance, x, y, nx, ny
//   physics.joint(a, b, type, ax, ay [, min, max]) -> joint (a = 0: the world;
//       type weld / revolute / prismatic / distance; anchor relative to b's
//       centre; min / max turn limits on)
//   physics.velocity_axis(e, 0 | 1) and physics.ray_distance(...) (-1 = miss):
//       one-value forms for graphs / C++ / C#
//   physics.unjoint(joint)              physics.joint_value(joint) -> deg / px
//   physics.bodies() -> number of rigid bodies

#include "runtime/World.h"

#include "lauxlib.h"
#include "lua.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace ts {
namespace tombstone {
namespace runtime {

namespace {

constexpr const char* kEntityMeta = "TombStone.Entity";  // see ScriptApi.cpp

World& W(lua_State* L) { return *static_cast<World*>(lua_touserdata(L, lua_upvalueindex(1))); }

std::uint64_t id_arg(lua_State* L, int idx) {
  if (const auto* u = static_cast<std::uint64_t*>(luaL_testudata(L, idx, kEntityMeta))) return *u;
  const lua_Number n = luaL_checknumber(L, idx);
  return n > 0 ? static_cast<std::uint64_t>(n) : 0;
}

float fnum(lua_State* L, int idx) {
  const lua_Number n = luaL_checknumber(L, idx);
  luaL_argcheck(L, std::isfinite(n) && std::fabs(n) < 1e9, idx, "number out of range");
  return static_cast<float>(n);
}

int apply(lua_State* L, const char* what) {
  const bool torque = what[0] == 't';
  lua_pushboolean(L, W(L).phys2d_apply(id_arg(L, 1), what, fnum(L, 2), torque ? 0.0f : fnum(L, 3)));
  return 1;
}
int apply_force(lua_State* L) { return apply(L, "force"); }
int apply_impulse(lua_State* L) { return apply(L, "impulse"); }
int apply_torque(lua_State* L) { return apply(L, "torque"); }

int set_velocity(lua_State* L) {
  lua_pushboolean(L, W(L).phys2d_set_velocity(id_arg(L, 1), fnum(L, 2), fnum(L, 3)));
  return 1;
}

int velocity(lua_State* L) {
  float x = 0, y = 0;
  W(L).phys2d_velocity(id_arg(L, 1), &x, &y);
  lua_pushnumber(L, x);
  lua_pushnumber(L, y);
  return 2;
}

int speed(lua_State* L) {
  float x = 0, y = 0;
  W(L).phys2d_velocity(id_arg(L, 1), &x, &y);
  lua_pushnumber(L, std::sqrt(x * x + y * y));
  return 1;
}

int raycast(lua_State* L) {
  physics2d::RayHit h;
  if (!W(L).phys2d_raycast(fnum(L, 1), fnum(L, 2), fnum(L, 3), fnum(L, 4),
                           static_cast<float>(luaL_optnumber(L, 5, 10000.0)), &h)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_pushinteger(L, static_cast<lua_Integer>(h.user));
  lua_pushnumber(L, h.distance);
  lua_pushnumber(L, h.point.x);
  lua_pushnumber(L, h.point.y);
  lua_pushnumber(L, h.normal.x);
  lua_pushnumber(L, h.normal.y);
  return 6;
}

// One-value forms for the plugin bridge (graphs, C++, C#).
int velocity_axis(lua_State* L) {
  const int axis = static_cast<int>(luaL_checkinteger(L, 2));
  lua_settop(L, 1);
  velocity(L);  // pushes 2 numbers
  lua_pushvalue(L, 2 + (axis < 0 ? 0 : axis > 1 ? 1 : axis));
  return 1;
}

int ray_distance(lua_State* L) {
  const int n = raycast(L);
  if (n == 1) {
    lua_pushnumber(L, -1);
    return 1;
  }
  lua_pushvalue(L, -(n - 1));
  return 1;
}

int joint(lua_State* L) {
  Joint2DData j;
  const std::uint64_t a = id_arg(L, 1), b = id_arg(L, 2);
  const std::string t = luaL_optstring(L, 3, "revolute");
  j.type = t == "weld"        ? Joint2DData::Type::Weld
           : t == "prismatic" ? Joint2DData::Type::Prismatic
           : t == "distance"  ? Joint2DData::Type::Distance
                              : Joint2DData::Type::Revolute;
  j.anchor_x = static_cast<float>(luaL_optnumber(L, 4, 0.0));
  j.anchor_y = static_cast<float>(luaL_optnumber(L, 5, 0.0));
  if (lua_isnumber(L, 6) && lua_isnumber(L, 7)) {
    j.limits = true;
    j.min = fnum(L, 6);
    j.max = fnum(L, 7);
  }
  std::string err;
  const std::uint32_t id = W(L).phys2d_joint(j, a, b, &err);
  if (!id) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  lua_pushinteger(L, id);
  return 1;
}

int unjoint(lua_State* L) {
  W(L).phys2d_unjoint(static_cast<std::uint32_t>(luaL_checkinteger(L, 1)));
  return 0;
}

int joint_value(lua_State* L) {
  lua_pushnumber(L, W(L).phys2d_joint_value(static_cast<std::uint32_t>(luaL_checkinteger(L, 1))));
  return 1;
}

int bodies(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(W(L).rigid_body_count()));
  return 1;
}

// 3D-shaped forms (graphs use one node for 2D and 3D): z arguments dropped.
int drop_arg(lua_State* L, int idx, lua_CFunction f) {
  if (lua_gettop(L) >= idx) lua_remove(L, idx);
  return f(L);
}
int raycast_xyz(lua_State* L) {  // x, y, z, dx, dy, dz, dist
  lua_remove(L, 3);
  return drop_arg(L, 5, raycast);
}
int ray_distance_xyz(lua_State* L) {
  lua_remove(L, 3);
  return drop_arg(L, 5, ray_distance);
}
int joint_xyz(lua_State* L) { return drop_arg(L, 6, joint); }  // a, b, type, ax, ay, az, min, max

}  // namespace

void World::open_physics_lua(lua_State* L, int sandbox) {
  const luaL_Reg fns[] = {{"apply_force", apply_force}, {"apply_impulse", apply_impulse},
                          {"apply_torque", apply_torque}, {"set_velocity", set_velocity},
                          {"velocity", velocity},       {"speed", speed}, {"velocity_axis", velocity_axis}, {"ray_distance", ray_distance},
                          {"raycast", raycast},         {"joint", joint},
                          {"unjoint", unjoint},         {"joint_value", joint_value},
                          {"bodies", bodies},           {"raycast_xyz", raycast_xyz},
                          {"ray_distance_xyz", ray_distance_xyz}, {"joint_xyz", joint_xyz},
                          {nullptr, nullptr}};
  lua_newtable(L);
  lua_pushlightuserdata(L, this);
  luaL_setfuncs(L, fns, 1);
  lua_setfield(L, sandbox, "physics");
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
