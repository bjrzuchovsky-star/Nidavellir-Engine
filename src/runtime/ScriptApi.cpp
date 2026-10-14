// The functions scripts call: entity handles (self, other, find(...)) and
// the shared globals (log, after, toast, spawn...). Everything goes through
// ScriptWorld; nothing here touches files, the OS or the clock.

#include "runtime/ScriptState.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace ts {
namespace tombstone {
namespace runtime {

namespace {

constexpr const char* kEntityMeta = "TombStone.Entity";
constexpr std::size_t kMaxKey = 64;
constexpr std::size_t kMaxLogText = 4096;
constexpr double kDefaultToastSeconds = 2.5;

using State = ScriptHost::State;

State& host(lua_State* L) { return *State::from(L); }

std::uint64_t check_entity(lua_State* L, int idx) {
  return *static_cast<std::uint64_t*>(luaL_checkudata(L, idx, kEntityMeta));
}

float check_coord(lua_State* L, int idx) {
  const double v = luaL_checknumber(L, idx);
  if (!std::isfinite(v) || std::fabs(v) > 1.0e7) {
    luaL_argerror(L, idx, "must be a finite number within +/-10,000,000");
  }
  return static_cast<float>(v);
}

std::string check_key(lua_State* L, int idx) {
  std::size_t n = 0;
  const char* s = luaL_checklstring(L, idx, &n);
  if (n == 0 || n > kMaxKey) {
    luaL_argerror(L, idx, "key must be 1 to 64 characters");
  }
  return std::string(s, n);
}

// Seconds -> whole ticks (at least one), rejecting nonsense.
std::uint64_t check_ticks(lua_State* L, int idx) {
  const double sec = luaL_checknumber(L, idx);
  if (!std::isfinite(sec) || sec < 0.0 || sec > 86400.0) {
    luaL_argerror(L, idx, "seconds must be between 0 and 86400");
  }
  const auto ticks = static_cast<std::uint64_t>(std::llround(sec * kScriptTickRate));
  return std::max<std::uint64_t>(1, ticks);
}

// All arguments, tostring'd and joined by spaces (like print).
std::string join_args(lua_State* L, int first) {
  std::string out;
  const int top = lua_gettop(L);
  for (int i = first; i <= top; ++i) {
    std::size_t n = 0;
    const char* s = luaL_tolstring(L, i, &n);
    if (i > first) out += ' ';
    out.append(s, n);
    lua_pop(L, 1);
    if (out.size() > kMaxLogText) {
      out.resize(kMaxLogText);
      out += "...";
      break;
    }
  }
  return out;
}

// The script line that called into us.
void caller(lua_State* L, std::string* file, int* line) {
  lua_Debug ar;
  if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar) && ar.source &&
      ar.source[0] == '@') {
    *file = ar.source + 1;
    *line = ar.currentline;
  }
}

int log_at(lua_State* L, LogLevel level) {
  State& s = host(L);
  std::string file;
  int line = 0;
  caller(L, &file, &line);
  s.log(level, join_args(L, 1), file, line, s.current ? s.current->entity : 0);
  return 0;
}

// --- Entity methods -----------------------------------------------------------

const Entity2D* live(lua_State* L, std::uint64_t id) {
  State& s = host(L);
  return s.world ? s.world->script_entity(id) : nullptr;
}

int e_id(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(check_entity(L, 1)));
  return 1;
}

int e_name(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  const Entity2D* e = live(L, id);
  lua_pushstring(L, e ? e->name.c_str() : "");
  return 1;
}

int e_alive(lua_State* L) {
  lua_pushboolean(L, live(L, check_entity(L, 1)) != nullptr);
  return 1;
}

int e_pos(lua_State* L) {
  const Entity2D* e = live(L, check_entity(L, 1));
  if (!e) return 0;
  lua_pushnumber(L, e->x);
  lua_pushnumber(L, e->y);
  return 2;
}

int e_center(lua_State* L) {
  const Entity2D* e = live(L, check_entity(L, 1));
  if (!e) return 0;
  lua_pushnumber(L, e->center_x());
  lua_pushnumber(L, e->center_y());
  return 2;
}

int e_size(lua_State* L) {
  const Entity2D* e = live(L, check_entity(L, 1));
  if (!e) return 0;
  lua_pushnumber(L, e->w);
  lua_pushnumber(L, e->h);
  return 2;
}

int e_set_pos(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  const float x = check_coord(L, 2);
  const float y = check_coord(L, 3);
  if (live(L, id)) host(L).world->script_move(id, x, y);
  return 0;
}

int e_move_by(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  const float dx = check_coord(L, 2);
  const float dy = check_coord(L, 3);
  if (const Entity2D* e = live(L, id)) {
    host(L).world->script_move(id, e->x + dx, e->y + dy);
  }
  return 0;
}

int e_show(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  if (live(L, id)) host(L).world->script_set_visible(id, true);
  return 0;
}

int e_hide(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  if (live(L, id)) host(L).world->script_set_visible(id, false);
  return 0;
}

int e_visible(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  lua_pushboolean(L, live(L, id) && host(L).world->script_visible(id));
  return 1;
}

int e_set_collider(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  luaL_checkany(L, 2);
  if (live(L, id)) host(L).world->script_set_collider(id, lua_toboolean(L, 2));
  return 0;
}

int e_collider_on(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  lua_pushboolean(L, live(L, id) && host(L).world->script_collider_on(id));
  return 1;
}

int e_play(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  const std::string clip = luaL_checkstring(L, 2);
  const bool hold = lua_toboolean(L, 3);
  lua_pushboolean(L, live(L, id) && host(L).world->script_play(id, clip, hold));
  return 1;
}

int e_release(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  if (live(L, id)) host(L).world->script_release(id);
  return 0;
}

int e_get(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  const std::string key = check_key(L, 2);
  const ScriptValue* v = live(L, id) ? host(L).world->script_get(id, key) : nullptr;
  if (v) {
    push_script_value(L, *v);
  } else {
    lua_settop(L, 3);  // the default (or nil)
  }
  return 1;
}

int e_set(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  const std::string key = check_key(L, 2);
  if (!live(L, id)) return 0;
  ScriptWorld& w = *host(L).world;
  if (lua_isnoneornil(L, 3)) {
    w.script_set(id, key, nullptr);
    return 0;
  }
  ScriptValue v;
  if (!to_script_value(L, 3, &v)) {
    return luaL_argerror(L, 3, "state holds booleans, numbers and strings only");
  }
  if (v.type == ScriptValue::Type::Number && !std::isfinite(v.number)) {
    return luaL_argerror(L, 3, "number must be finite");
  }
  if (v.text.size() > ScriptData::kMaxTextLength) {
    v.text.resize(ScriptData::kMaxTextLength);
  }
  w.script_set(id, key, &v);  // the world caps keys per entity
  return 0;
}

int e_destroy(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  if (live(L, id)) host(L).world->script_destroy(id);
  return 0;
}

int e_is_player(lua_State* L) {
  const Entity2D* e = live(L, check_entity(L, 1));
  lua_pushboolean(L, e && e->player.has_value());
  return 1;
}

int e_slot(lua_State* L) {
  const Entity2D* e = live(L, check_entity(L, 1));
  if (!e || !e->player) return 0;
  lua_pushinteger(L, e->player->slot + 1);
  return 1;
}

int e_toast(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  const std::string text = luaL_checkstring(L, 2);
  const double sec = luaL_optnumber(L, 3, kDefaultToastSeconds);
  const Entity2D* e = live(L, id);
  if (!e) return 0;
  State& s = host(L);
  s.world->script_toast(e->player ? e->player->slot : -1, text, sec,
                        s.current ? s.current->entity : id);
  return 0;
}

int e_spawned(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  lua_pushboolean(L, live(L, id) && host(L).world->script_spawned(id));
  return 1;
}

int e_eq(lua_State* L) {
  const auto* a = static_cast<std::uint64_t*>(luaL_testudata(L, 1, kEntityMeta));
  const auto* b = static_cast<std::uint64_t*>(luaL_testudata(L, 2, kEntityMeta));
  lua_pushboolean(L, a && b && *a == *b);
  return 1;
}

int e_tostring(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  const Entity2D* e = live(L, id);
  lua_pushfstring(L, "%s #%I", e ? e->name.c_str() : "(gone)",
                  static_cast<lua_Integer>(id));
  return 1;
}

// --- Globals ------------------------------------------------------------------

int g_log(lua_State* L) { return log_at(L, LogLevel::Info); }
int g_warn(lua_State* L) { return log_at(L, LogLevel::Warn); }

int add_timer(lua_State* L, bool repeat) {
  const std::uint64_t ticks = check_ticks(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  State& s = host(L);
  if (!s.world || !s.current) {
    return luaL_error(L, "timers can only be set while the world runs (not at load)");
  }
  if (s.timers.size() >= ScriptHost::kMaxTimers) {
    return luaL_error(L, "too many timers (limit %d)",
                      static_cast<int>(ScriptHost::kMaxTimers));
  }
  ScriptTimer t;
  t.id = s.next_timer++;
  t.entity = s.current->entity;
  t.due = s.world->script_tick() + ticks;
  t.period = repeat ? ticks : 0;
  lua_pushvalue(L, 2);
  t.fn = luaL_ref(L, LUA_REGISTRYINDEX);
  s.timers.push_back(t);
  lua_pushinteger(L, static_cast<lua_Integer>(t.id));
  return 1;
}

int g_after(lua_State* L) { return add_timer(L, false); }
int g_every(lua_State* L) { return add_timer(L, true); }

int g_cancel(lua_State* L) {
  const lua_Integer id = luaL_checkinteger(L, 1);
  State& s = host(L);
  for (auto it = s.timers.begin(); it != s.timers.end(); ++it) {
    if (static_cast<lua_Integer>(it->id) == id && s.current &&
        it->entity == s.current->entity) {
      luaL_unref(L, LUA_REGISTRYINDEX, it->fn);
      s.timers.erase(it);
      lua_pushboolean(L, 1);
      return 1;
    }
  }
  lua_pushboolean(L, 0);
  return 1;
}

int g_toast(lua_State* L) {
  const std::string text = luaL_checkstring(L, 1);
  const double sec = luaL_optnumber(L, 2, kDefaultToastSeconds);
  State& s = host(L);
  if (s.world) {
    s.world->script_toast(-1, text, sec, s.current ? s.current->entity : 0);
  }
  return 0;
}

int g_find(lua_State* L) {
  const std::string name = luaL_checkstring(L, 1);
  State& s = host(L);
  push_entity(L, s.world ? s.world->script_find(name) : 0);
  return 1;
}

int g_player(lua_State* L) {
  const lua_Integer slot = luaL_optinteger(L, 1, 1);
  State& s = host(L);
  const bool ok = s.world && slot >= 1 && slot <= 64;
  push_entity(L, ok ? s.world->script_player(static_cast<int>(slot - 1)) : 0);
  return 1;
}

int g_spawn(lua_State* L) {
  const std::string tmpl = luaL_checkstring(L, 1);
  const float x = check_coord(L, 2);
  const float y = check_coord(L, 3);
  State& s = host(L);
  push_entity(L, s.world ? s.world->script_spawn(tmpl, x, y) : 0);
  return 1;
}

int g_duplicate(lua_State* L) {
  const std::uint64_t id = check_entity(L, 1);
  const Entity2D* e = live(L, id);
  if (!e) {
    lua_pushnil(L);
    return 1;
  }
  const float x = lua_isnoneornil(L, 2) ? e->x : check_coord(L, 2);
  const float y = lua_isnoneornil(L, 3) ? e->y : check_coord(L, 3);
  push_entity(L, host(L).world->script_duplicate(id, x, y));
  return 1;
}

int g_tick(lua_State* L) {
  State& s = host(L);
  lua_pushinteger(L, s.world ? static_cast<lua_Integer>(s.world->script_tick()) : 0);
  return 1;
}

int g_time(lua_State* L) {
  State& s = host(L);
  const double t = s.world ? static_cast<double>(s.world->script_tick()) : 0.0;
  lua_pushnumber(L, t / kScriptTickRate);
  return 1;
}

// math.random / math.randomseed on a replayable generator (splitmix64)
// seeded the same for every ride: same scene + same input = same rolls.
std::uint64_t next_random(State& s) {
  std::uint64_t z = (s.rng += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

int m_random(lua_State* L) {
  State& s = host(L);
  const double unit =
      static_cast<double>(next_random(s) >> 11) * (1.0 / 9007199254740992.0);
  const int n = lua_gettop(L);
  if (n == 0) {
    lua_pushnumber(L, unit);
    return 1;
  }
  lua_Integer lo = 1;
  lua_Integer hi = 0;
  if (n == 1) {
    hi = luaL_checkinteger(L, 1);
  } else {
    lo = luaL_checkinteger(L, 1);
    hi = luaL_checkinteger(L, 2);
  }
  luaL_argcheck(L, lo <= hi && hi - lo < (lua_Integer{1} << 52), n,
                "interval is empty or too large");
  const auto span = static_cast<double>(hi - lo + 1);
  lua_pushinteger(L, lo + static_cast<lua_Integer>(unit * span));
  return 1;
}

int m_randomseed(lua_State* L) {
  host(L).rng = static_cast<std::uint64_t>(luaL_optinteger(L, 1, 0));
  return 0;
}

// ---- audio.* ---------------------------------------------------------------
// audio.play(clip [, {volume, pitch, variation, bus, loop, at = entity,
//   x, y}]) / audio.music(clip [, fade]) / audio.playlist({...} [, fade])
// / audio.stop_music([fade]) / audio.set_volume(bus, v) / audio.stop([fade]).
// Queued for the host (and sent to every rider in multiplayer).

float opt_field(lua_State* L, int t, const char* key, float fallback) {
  if (t == 0) return fallback;
  lua_getfield(L, t, key);
  const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : fallback;
  lua_pop(L, 1);
  return v == v ? v : fallback;
}

std::string clip_arg(lua_State* L, int idx) {
  std::size_t n = 0;
  const char* c = luaL_checklstring(L, idx, &n);
  if (n == 0 || n > 512) luaL_argerror(L, idx, "clip name must be 1..512 characters");
  return std::string(c, n);
}

bool known_bus(const std::string& b) {
  return b == "master" || b == "music" || b == "sfx" || b == "ui" || b == "voice";
}

int send_sound(lua_State* L, SoundEvent ev) {
  State& s = host(L);
  if (!s.world) {
    return luaL_error(L, "audio can only be used while the world runs (not at load)");
  }
  if (ev.entity == 0 && s.current) ev.entity = s.current->entity;
  s.world->script_sound(std::move(ev));
  return 0;
}

int a_play(lua_State* L) {
  SoundEvent ev;
  ev.clip = clip_arg(L, 1);
  const int t = lua_istable(L, 2) ? 2 : 0;
  ev.volume = std::clamp(opt_field(L, t, "volume", 1.0f), 0.0f, 2.0f);
  ev.pitch = std::clamp(opt_field(L, t, "pitch", 1.0f), 0.1f, 4.0f);
  ev.variation = std::clamp(opt_field(L, t, "variation", 0.0f), 0.0f, 0.9f);
  if (t) {
    lua_getfield(L, t, "bus");
    if (lua_isstring(L, -1)) ev.bus = lua_tostring(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, t, "loop");
    ev.loop = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, t, "at");
    if (!lua_isnil(L, -1)) {
      const std::uint64_t id = check_entity(L, lua_gettop(L));
      if (const Entity2D* e = live(L, id)) {
        ev.positional = true;
        ev.entity = id;
        ev.x = e->center_x();
        ev.y = e->center_y();
      }
    }
    lua_pop(L, 1);
    lua_getfield(L, t, "x");
    const bool has_x = lua_isnumber(L, -1) != 0;
    lua_pop(L, 1);
    if (has_x) {
      ev.positional = true;
      ev.x = opt_field(L, t, "x", 0.0f);
      ev.y = opt_field(L, t, "y", 0.0f);
      ev.z = opt_field(L, t, "z", 0.0f);
    }
  }
  if (!known_bus(ev.bus)) {
    return luaL_error(L, "unknown bus '%s' (master, music, sfx, ui, voice)", ev.bus.c_str());
  }
  return send_sound(L, std::move(ev));
}

float fade_arg(lua_State* L, int idx, double fallback) {
  return std::clamp(static_cast<float>(luaL_optnumber(L, idx, fallback)), 0.0f, 30.0f);
}

int a_music(lua_State* L) {
  SoundEvent ev;
  ev.kind = SoundEvent::Kind::Music;
  ev.clip = clip_arg(L, 1);
  ev.bus = "music";
  ev.fade = fade_arg(L, 2, 1.0);
  ev.loop = true;
  return send_sound(L, std::move(ev));
}

int a_playlist(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  SoundEvent ev;
  ev.kind = SoundEvent::Kind::Playlist;
  ev.bus = "music";
  ev.fade = fade_arg(L, 2, 1.0);
  const lua_Integer n = luaL_len(L, 1);
  if (n > 64) return luaL_error(L, "a playlist holds at most 64 tracks");
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_geti(L, 1, i);
    ev.list.push_back(clip_arg(L, lua_gettop(L)));
    lua_pop(L, 1);
  }
  return send_sound(L, std::move(ev));
}

int a_stop_music(lua_State* L) {
  SoundEvent ev;
  ev.kind = SoundEvent::Kind::StopMusic;
  ev.bus = "music";
  ev.fade = fade_arg(L, 1, 1.0);
  return send_sound(L, std::move(ev));
}

int a_set_volume(lua_State* L) {
  SoundEvent ev;
  ev.kind = SoundEvent::Kind::Volume;
  ev.bus = luaL_checkstring(L, 1);
  ev.volume = std::clamp(static_cast<float>(luaL_checknumber(L, 2)), 0.0f, 2.0f);
  if (!known_bus(ev.bus)) {
    return luaL_error(L, "unknown bus '%s' (master, music, sfx, ui, voice)", ev.bus.c_str());
  }
  return send_sound(L, std::move(ev));
}

int a_stop(lua_State* L) {
  SoundEvent ev;
  ev.kind = SoundEvent::Kind::Stop;
  ev.fade = fade_arg(L, 1, 0.1);
  return send_sound(L, std::move(ev));
}

}  // namespace

void push_entity(lua_State* L, std::uint64_t id) {
  if (id == 0) {
    lua_pushnil(L);
    return;
  }
  State& s = host(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, s.handles);
  if (lua_rawgeti(L, -1, static_cast<lua_Integer>(id)) != LUA_TNIL) {
    lua_remove(L, -2);
    return;
  }
  lua_pop(L, 1);
  auto* p = static_cast<std::uint64_t*>(lua_newuserdatauv(L, sizeof(std::uint64_t), 0));
  *p = id;
  luaL_setmetatable(L, kEntityMeta);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, static_cast<lua_Integer>(id));
  lua_remove(L, -2);
}

void push_script_value(lua_State* L, const ScriptValue& v) {
  switch (v.type) {
    case ScriptValue::Type::Bool:
      lua_pushboolean(L, v.flag);
      break;
    case ScriptValue::Type::Text:
      lua_pushlstring(L, v.text.data(), v.text.size());
      break;
    case ScriptValue::Type::Number:
    default:
      // Whole numbers come back as integers so logs read "3", not "3.0".
      if (std::isfinite(v.number) && v.number == std::floor(v.number) &&
          std::fabs(v.number) < 9007199254740992.0) {
        lua_pushinteger(L, static_cast<lua_Integer>(v.number));
      } else {
        lua_pushnumber(L, v.number);
      }
      break;
  }
}

bool to_script_value(lua_State* L, int idx, ScriptValue* out) {
  switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN:
      *out = ScriptValue::of_bool(lua_toboolean(L, idx) != 0);
      return true;
    case LUA_TNUMBER:
      *out = ScriptValue::of_number(static_cast<double>(lua_tonumber(L, idx)));
      return true;
    case LUA_TSTRING: {
      std::size_t n = 0;
      const char* s = lua_tolstring(L, idx, &n);
      *out = ScriptValue::of_text(std::string(s, n));
      return true;
    }
    default:
      return false;
  }
}

void open_script_api(lua_State* L) {
  // Entity handles: full userdata holding the id, methods via __index.
  const luaL_Reg methods[] = {
      {"id", e_id},           {"name", e_name},
      {"alive", e_alive},     {"pos", e_pos},
      {"center", e_center},   {"size", e_size},
      {"set_pos", e_set_pos}, {"move_by", e_move_by},
      {"show", e_show},       {"hide", e_hide},
      {"visible", e_visible}, {"set_collider", e_set_collider},
      {"collider_on", e_collider_on},
      {"play", e_play},       {"release", e_release},
      {"get", e_get},         {"set", e_set},
      {"destroy", e_destroy}, {"is_player", e_is_player},
      {"slot", e_slot},       {"toast", e_toast},
      {"spawned", e_spawned}, {nullptr, nullptr}};
  luaL_newmetatable(L, kEntityMeta);
  luaL_newlib(L, methods);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, e_eq);
  lua_setfield(L, -2, "__eq");
  lua_pushcfunction(L, e_tostring);
  lua_setfield(L, -2, "__tostring");
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "__metatable");
  lua_pop(L, 1);

  const luaL_Reg globals[] = {
      {"log", g_log},       {"print", g_log},         {"warn", g_warn},
      {"after", g_after},   {"every", g_every},       {"cancel", g_cancel},
      {"toast", g_toast},   {"find", g_find},         {"player", g_player},
      {"spawn", g_spawn},   {"duplicate", g_duplicate},
      {"tick", g_tick},     {"time", g_time},         {nullptr, nullptr}};
  luaL_setfuncs(L, globals, 0);

  const luaL_Reg audio_fns[] = {
      {"play", a_play},           {"music", a_music},
      {"playlist", a_playlist},   {"stop_music", a_stop_music},
      {"set_volume", a_set_volume}, {"stop", a_stop},
      {nullptr, nullptr}};
  luaL_newlib(L, audio_fns);
  lua_setfield(L, -2, "audio");

  lua_getfield(L, -1, LUA_MATHLIBNAME);
  lua_pushcfunction(L, m_random);
  lua_setfield(L, -2, "random");
  lua_pushcfunction(L, m_randomseed);
  lua_setfield(L, -2, "randomseed");
  lua_pop(L, 1);
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
