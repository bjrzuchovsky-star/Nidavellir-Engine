#pragma once

// The Lua `weather` table over a weather Sim (2D plugin and 3D extension):
//   weather.set(kind [, fade])        kind: clear rain snow dust fog storm
//   weather.get() -> now, target, blend
//   weather.wind() -> x, z, strength  weather.set_wind(deg, strength) (< 0: auto)
//   weather.time() -> 0..1            weather.set_time(t)
//   weather.lightning()               weather.intensity(k)
//   weather.amount(name) -> 0..1      rain snow dust fog storm cloud flash daylight

struct lua_State;

namespace ts {
namespace tombstone {
namespace weather {

class Sim;
// Leaves the table on the stack. `sim` must outlive the Lua state's use.
void push_weather_lua(lua_State* L, Sim* sim);

}  // namespace weather
}  // namespace tombstone
}  // namespace ts
