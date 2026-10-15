#include "weather/WeatherLua.h"

#include "weather/Weather.h"

#include "lauxlib.h"
#include "lua.h"

#include <cmath>
#include <string>

namespace ts {
namespace tombstone {
namespace weather {

namespace {
Sim& S(lua_State* L) { return *static_cast<Sim*>(lua_touserdata(L, lua_upvalueindex(1))); }

float fnum(lua_State* L, int i) {
  const lua_Number n = luaL_checknumber(L, i);
  luaL_argcheck(L, std::isfinite(n), i, "number expected");
  return static_cast<float>(n);
}

int l_set(lua_State* L) {
  Kind k;
  if (!kind_from(luaL_checkstring(L, 1), &k)) return luaL_argerror(L, 1, "clear / rain / snow / dust / fog / storm");
  const float fade = lua_isnoneornil(L, 2) ? -1.0f : fnum(L, 2);
  S(L).set(k, fade);
  if (S(L).on_script_set) S(L).on_script_set(k, fade);
  return 0;
}
int l_get(lua_State* L) {
  const State& s = S(L).state();
  lua_pushstring(L, kind_name(s.blend >= 0.5f ? s.target : s.kind));
  lua_pushstring(L, kind_name(s.target));
  lua_pushnumber(L, s.blend);
  return 3;
}
int l_wind(lua_State* L) {
  const State& s = S(L).state();
  lua_pushnumber(L, s.wind_dir[0] * s.wind);
  lua_pushnumber(L, s.wind_dir[1] * s.wind);
  lua_pushnumber(L, s.wind);
  return 3;
}
int l_set_wind(lua_State* L) {
  S(L).set_wind(fnum(L, 1), fnum(L, 2));
  return 0;
}
int l_time(lua_State* L) {
  lua_pushnumber(L, S(L).state().time_of_day);
  return 1;
}
int l_set_time(lua_State* L) {
  S(L).set_time_of_day(fnum(L, 1));
  return 0;
}
int l_lightning(lua_State* L) {
  S(L).strike();
  return 0;
}
int l_intensity(lua_State* L) {
  S(L).set_intensity(fnum(L, 1));
  return 0;
}
int l_amount(lua_State* L) {
  const State& s = S(L).state();
  const Mix& m = s.mix;
  const std::string n = luaL_checkstring(L, 1);
  float v = 0;
  if (n == "rain") v = m.rain;
  else if (n == "snow") v = m.snow;
  else if (n == "dust") v = m.dust;
  else if (n == "fog") v = m.fog;
  else if (n == "storm") v = m.storm;
  else if (n == "cloud") v = m.cloud;
  else if (n == "flash") v = s.flash;
  else if (n == "daylight") v = s.daylight;
  else return luaL_argerror(L, 1, "rain / snow / dust / fog / storm / cloud / flash / daylight");
  lua_pushnumber(L, v);
  return 1;
}
}  // namespace

void push_weather_lua(lua_State* L, Sim* sim) {
  const luaL_Reg fns[] = {{"set", l_set},           {"get", l_get},           {"wind", l_wind},
                          {"set_wind", l_set_wind}, {"time", l_time},         {"set_time", l_set_time},
                          {"lightning", l_lightning}, {"intensity", l_intensity}, {"amount", l_amount},
                          {nullptr, nullptr}};
  lua_newtable(L);
  lua_pushlightuserdata(L, sim);
  luaL_setfuncs(L, fns, 1);
}

}  // namespace weather
}  // namespace tombstone
}  // namespace ts
