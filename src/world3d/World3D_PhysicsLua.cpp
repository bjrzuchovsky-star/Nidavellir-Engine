// The Lua `physics` table of a 3D world (Jolt bodies). Graphs, C++ and C#
// reach the same functions through the plugin call bridge. Entities are
// handles or ids; units metres, m / s, degrees; y is up.
//
//   physics.apply_force(e, x, y, z)     physics.apply_impulse(e, x, y, z)
//   physics.apply_torque(e, x, y, z)    physics.set_velocity(e, x, y, z)
//   physics.velocity(e) -> x, y, z      physics.speed(e) -> m / s
//   physics.raycast(x, y, z, dx, dy, dz, dist) -> hit entity (or false),
//       distance, x, y, z, nx, ny, nz
//   physics.joint(a, b, type, ax, ay, az [, min, max]) -> joint (a = 0: the
//       world; type fixed / hinge / slider / distance / ball, 2D names weld /
//       revolute / prismatic work too; anchor local to b; hinge axis y)
//   physics.velocity_axis(e, 0 | 1 | 2) and physics.ray_distance(...) (-1 = miss):
//       one-value forms for graphs / C++ / C#
//   physics.unjoint(joint)              physics.bodies() -> rigid bodies

#include "world3d/World3D.h"

#include "lauxlib.h"
#include "lua.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace ts {
namespace tombstone {
namespace world3d {

namespace {

constexpr const char* kEntityMeta = "TombStone.Entity";  // see ScriptApi.cpp

World3D& W(lua_State* L) { return *static_cast<World3D*>(lua_touserdata(L, lua_upvalueindex(1))); }

std::uint64_t id_arg(lua_State* L, int idx) {
  if (const auto* u = static_cast<std::uint64_t*>(luaL_testudata(L, idx, kEntityMeta))) return *u;
  const lua_Number n = luaL_checknumber(L, idx);
  return n > 0 ? static_cast<std::uint64_t>(n) : 0;
}

float fnum(lua_State* L, int idx) {
  const lua_Number n = luaL_checknumber(L, idx);
  luaL_argcheck(L, std::isfinite(n) && std::fabs(n) < 1e7, idx, "number out of range");
  return static_cast<float>(n);
}

render3d::Vec3 v3(lua_State* L, int idx) { return {fnum(L, idx), fnum(L, idx + 1), fnum(L, idx + 2)}; }

int apply(lua_State* L, const char* what) {
  lua_pushboolean(L, W(L).phys_apply(id_arg(L, 1), what, v3(L, 2)));
  return 1;
}
int apply_force(lua_State* L) { return apply(L, "force"); }
int apply_impulse(lua_State* L) { return apply(L, "impulse"); }
int apply_torque(lua_State* L) { return apply(L, "torque"); }

int set_velocity(lua_State* L) {
  lua_pushboolean(L, W(L).phys_set_velocity(id_arg(L, 1), v3(L, 2)));
  return 1;
}

int velocity(lua_State* L) {
  const render3d::Vec3 v = W(L).phys_velocity(id_arg(L, 1));
  lua_pushnumber(L, v.x);
  lua_pushnumber(L, v.y);
  lua_pushnumber(L, v.z);
  return 3;
}

int speed(lua_State* L) {
  lua_pushnumber(L, render3d::length(W(L).phys_velocity(id_arg(L, 1))));
  return 1;
}

int raycast(lua_State* L) {
  physics3d::RayHit h;
  render3d::Vec3 d = v3(L, 4);
  const float len = render3d::length(d);
  if (len < 1e-6f) return luaL_argerror(L, 4, "zero direction");
  d = d * (1.0f / len);
  if (!W(L).phys_raycast(v3(L, 1), d, static_cast<float>(luaL_optnumber(L, 7, 1000.0)), &h)) {
    lua_pushboolean(L, 0);
    return 1;
  }
  lua_pushinteger(L, static_cast<lua_Integer>(h.user));
  lua_pushnumber(L, h.distance);
  lua_pushnumber(L, h.point.x);
  lua_pushnumber(L, h.point.y);
  lua_pushnumber(L, h.point.z);
  lua_pushnumber(L, h.normal.x);
  lua_pushnumber(L, h.normal.y);
  lua_pushnumber(L, h.normal.z);
  return 8;
}

// One-value forms for the plugin bridge (graphs, C++, C#).
int velocity_axis(lua_State* L) {
  const int axis = static_cast<int>(luaL_checkinteger(L, 2));
  lua_settop(L, 1);
  velocity(L);  // pushes 3 numbers
  lua_pushvalue(L, 2 + (axis < 0 ? 0 : axis > 2 ? 2 : axis));
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
  using T = scene3d::Joint3D::Type;
  scene3d::Joint3D j;
  const std::uint64_t a = id_arg(L, 1), b = id_arg(L, 2);
  const std::string t = luaL_optstring(L, 3, "hinge");
  j.type = t == "fixed" || t == "weld"          ? T::Fixed
           : t == "slider" || t == "prismatic"  ? T::Slider
           : t == "distance"                    ? T::Distance
           : t == "ball"                        ? T::Ball
                                                : T::Hinge;
  j.anchor = {static_cast<float>(luaL_optnumber(L, 4, 0.0)), static_cast<float>(luaL_optnumber(L, 5, 0.0)),
              static_cast<float>(luaL_optnumber(L, 6, 0.0))};
  if (lua_isnumber(L, 7) && lua_isnumber(L, 8)) {
    j.limits = true;
    j.min = fnum(L, 7);
    j.max = fnum(L, 8);
  }
  std::string err;
  const physics3d::JointId id = W(L).phys_joint(j, a, b, &err);
  if (!id) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, err.c_str());
    return 2;
  }
  lua_pushinteger(L, static_cast<lua_Integer>(id));
  return 1;
}

int unjoint(lua_State* L) {
  W(L).phys_unjoint(static_cast<physics3d::JointId>(luaL_checkinteger(L, 1)));
  return 0;
}

int bodies(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(W(L).physics_bodies()));
  return 1;
}

}  // namespace

void World3D::open_physics_lua(lua_State* L, int sandbox) {
  const luaL_Reg fns[] = {{"apply_force", apply_force}, {"apply_impulse", apply_impulse},
                          {"apply_torque", apply_torque}, {"set_velocity", set_velocity},
                          {"velocity", velocity},       {"speed", speed}, {"velocity_axis", velocity_axis}, {"ray_distance", ray_distance},
                          {"raycast", raycast},         {"joint", joint},
                          {"unjoint", unjoint},         {"bodies", bodies},
                          {"raycast_xyz", raycast},     {"ray_distance_xyz", ray_distance},
                          {"joint_xyz", joint},
                          {nullptr, nullptr}};
  lua_newtable(L);
  lua_pushlightuserdata(L, this);
  luaL_setfuncs(L, fns, 1);
  lua_setfield(L, sandbox, "physics");
}

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
