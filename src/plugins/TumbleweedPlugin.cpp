#include "plugins/TumbleweedPlugin.h"

#include "plugin/Catalog.h"
#include "runtime/World.h"

// Lua is built as C++ (cmake/FetchLua.cmake): no extern "C".
#include "lauxlib.h"
#include "lua.h"

#include <cctype>
#include <cmath>

namespace ts {
namespace tombstone {
namespace plugins {

namespace {

TumbleweedPlugin* self(lua_State* L) {
  return static_cast<TumbleweedPlugin*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// tumbleweed.wind() -> px/s;  tumbleweed.wind(px_per_s) sets it (clamped).
int l_wind(lua_State* L) {
  TumbleweedPlugin* p = self(L);
  if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
    p->set_wind(static_cast<float>(luaL_checknumber(L, 1)));
  }
  lua_pushnumber(L, p->wind());
  return 1;
}

int l_count(lua_State* L) {
  lua_pushinteger(L, self(L)->count());
  return 1;
}

int l_gusts(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(self(L)->gusts()));
  return 1;
}

}  // namespace

const plugin::PluginInfo& TumbleweedPlugin::static_info() {
  static const plugin::PluginInfo info = [] {
    plugin::PluginInfo i;
    i.id = "ts.tumbleweed";
    i.name = "Tumbleweeds";
    i.description =
        "Name a prop Tumbleweed* and it rolls with the wind. Sample gameplay "
        "plugin: a system, a Lua table, a component and an asset type.";
    i.author = "TombStone";
    i.version = plugin::Version{1, 0, 0};
    i.category = plugin::PluginCategory::Sample;
    i.project_types = plugin::kType2D;
    i.default_enabled = false;
    return i;
  }();
  return info;
}

bool TumbleweedPlugin::is_tumbleweed(const std::string& name) {
  static const char kPrefix[] = "tumbleweed";
  if (name.size() < sizeof(kPrefix) - 1) return false;
  for (std::size_t i = 0; i + 1 < sizeof(kPrefix); ++i) {
    if (std::tolower(static_cast<unsigned char>(name[i])) != kPrefix[i]) {
      return false;
    }
  }
  return true;
}

void TumbleweedPlugin::set_wind(float px_per_s) {
  if (!std::isfinite(px_per_s)) return;
  if (px_per_s > 400.0f) px_per_s = 400.0f;
  if (px_per_s < -400.0f) px_per_s = -400.0f;
  wind_ = px_per_s;
}

void TumbleweedPlugin::on_register(plugin::Registrar& reg) {
  reg.add_component("Tumbleweed",
                    "Rolls with the wind during a ride (name the entity "
                    "Tumbleweed...)");
  reg.add_system("tumbleweed.roll", [this](runtime::World& w, double dt) {
    roll(w, dt);
  });
  reg.add_lua_api("tumbleweed", [this](lua_State* L) {
    lua_newtable(L);
    const luaL_Reg fns[] = {{"wind", &l_wind}, {"count", &l_count},
                            {"gusts", &l_gusts}, {nullptr, nullptr}};
    lua_pushlightuserdata(L, this);
    luaL_setfuncs(L, fns, 1);
  });
  reg.add_asset_type({".tumble.json", "Tumbleweed spawn table"});
}

void TumbleweedPlugin::on_play_begin(runtime::World& /*world*/) {
  rolling_.clear();
  gusts_ = 0;
  wind_ = kDefaultWind;
}

void TumbleweedPlugin::on_play_end() { rolling_.clear(); }

void TumbleweedPlugin::roll(runtime::World& w, double dt) {
  bool any = false;
  for (const runtime::Actor& a : w.actors()) {
    if (a.data.player || !is_tumbleweed(a.data.name)) continue;
    auto it = rolling_.find(a.data.id);
    if (it == rolling_.end()) {
      Roll r;
      r.origin_x = a.data.x;
      r.base_y = a.data.y;
      r.phase = static_cast<float>(a.data.id % 7) * 0.9f;
      it = rolling_.emplace(a.data.id, r).first;
    }
    Roll& r = it->second;
    r.t += dt;
    float x = a.data.x + wind_ * static_cast<float>(dt);
    if (std::fabs(x - r.origin_x) > kRollDistance) x = r.origin_x;
    const float y =
        r.base_y - std::fabs(std::sin(static_cast<float>(r.t) * 5.0f + r.phase)) * kBob;
    w.script_move(a.data.id, x, y);
    any = true;
  }
  if (any) ++gusts_;
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts

TS_PLUGIN_STATIC(tumbleweed, ::ts::tombstone::plugins::TumbleweedPlugin)
