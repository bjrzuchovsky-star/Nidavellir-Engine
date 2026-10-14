// 3D methods on script entity handles: pos3 / set_pos3 / move3 / yaw /
// set_yaw / spin / scale3 / set_scale3. Added to the shared handle
// metatable when a 3D world builds its script sandbox.

#include "world3d/World3D.h"

#include "runtime/ScriptState.h"

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

// ---- vr table ----------------------------------------------------------------

int side(lua_State* L, int idx) {
  if (lua_type(L, idx) == LUA_TNUMBER) return static_cast<int>(lua_tointeger(L, idx)) == 0 ? 0 : 1;
  const char* s = luaL_optstring(L, idx, "right");
  return (s[0] == 'l' || s[0] == 'L') ? kLeftHand : kRightHand;
}

float yaw_of(const VrPose& p) {
  const render3d::Vec3 f = p.forward();
  return render3d::degrees(std::atan2(-f.x, -f.z));
}

int vr_active(lua_State* L) {
  lua_pushboolean(L, world(L).vr() && world(L).running());
  return 1;
}

int vr_head(lua_State* L) {
  const VrPose h = world(L).head();
  lua_pushnumber(L, h.position.x);
  lua_pushnumber(L, h.position.y);
  lua_pushnumber(L, h.position.z);
  lua_pushnumber(L, yaw_of(h));
  return 4;
}

// x, y, z, trigger, squeeze of the grip.
int vr_hand(lua_State* L) {
  const VrHandState& h = world(L).hand(side(L, 1));
  lua_pushnumber(L, h.grip.position.x);
  lua_pushnumber(L, h.grip.position.y);
  lua_pushnumber(L, h.grip.position.z);
  lua_pushnumber(L, h.trigger);
  lua_pushnumber(L, h.squeeze);
  return 5;
}

int vr_held(lua_State* L) {
  runtime::push_entity(L, world(L).hand(side(L, 1)).held);
  return 1;
}

int vr_pointed(lua_State* L) {
  runtime::push_entity(L, world(L).hand(side(L, 1)).pointed);
  return 1;
}

int vr_rig(lua_State* L) {
  const World3D& w = world(L);
  lua_pushnumber(L, w.rig_position().x);
  lua_pushnumber(L, w.rig_position().y);
  lua_pushnumber(L, w.rig_position().z);
  lua_pushnumber(L, w.rig_yaw());
  return 4;
}

int vr_teleport(lua_State* L) {
  if (world(L).vr()) world(L).vr_teleport_to({num(L, 1), num(L, 2), num(L, 3)});
  return 0;
}

int vr_haptic_fn(lua_State* L) {
  const lua_Number amp = luaL_optnumber(L, 2, 0.5);
  const lua_Number secs = luaL_optnumber(L, 3, 0.05);
  world(L).vr_haptic(side(L, 1), static_cast<float>(amp), static_cast<float>(secs));
  return 0;
}

}  // namespace

void World3D::script_open_extensions(lua_State* L, int sandbox) {
  // The vr table (works in flat 3D too: vr.active() is false there).
  if (sandbox != 0) {
    const int sb = lua_absindex(L, sandbox);
    const luaL_Reg vr_fns[] = {{"active", vr_active},     {"head", vr_head},
                               {"hand", vr_hand},         {"held", vr_held},
                               {"pointed", vr_pointed},   {"rig", vr_rig},
                               {"teleport", vr_teleport}, {"haptic", vr_haptic_fn},
                               {nullptr, nullptr}};
    lua_newtable(L);
    lua_pushlightuserdata(L, this);
    luaL_setfuncs(L, vr_fns, 1);
    lua_setfield(L, sb, "vr");
  }
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
