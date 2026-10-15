// Lua faces of the story runtime: story / quests / dialogue / saves.
// Every function reads the runtime through the slot upvalue; with no ride
// they return nil / false instead of erroring.

#include "story/StoryRuntime.h"

#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <string>

namespace ts {
namespace tombstone {
namespace story {

namespace {

StoryRuntime* rt(lua_State* L) {
  auto** slot = static_cast<StoryRuntime**>(lua_touserdata(L, lua_upvalueindex(1)));
  StoryRuntime* r = slot ? *slot : nullptr;
  return r && r->running() ? r : nullptr;
}

void push_var(lua_State* L, const VarValue& v) {
  switch (v.kind) {
    case VarValue::Kind::Bool: lua_pushboolean(L, v.b); break;
    case VarValue::Kind::Number: lua_pushnumber(L, v.n); break;
    case VarValue::Kind::String: lua_pushlstring(L, v.s.data(), v.s.size()); break;
  }
}

bool to_var(lua_State* L, int idx, VarValue* out) {
  switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: *out = VarValue::boolean(lua_toboolean(L, idx) != 0); return true;
    case LUA_TNUMBER: *out = VarValue::number(lua_tonumber(L, idx)); return true;
    case LUA_TSTRING: *out = VarValue::string(lua_tostring(L, idx)); return true;
    default: return false;
  }
}

int ok_or_err(lua_State* L, bool ok, const std::string& err) {
  lua_pushboolean(L, ok);
  if (ok || err.empty()) return 1;
  lua_pushlstring(L, err.data(), err.size());
  return 2;
}

void set_fns(lua_State* L, StoryRuntime** slot, const luaL_Reg* fns) {
  lua_newtable(L);
  lua_pushlightuserdata(L, slot);
  luaL_setfuncs(L, fns, 1);
}

// ---- story ---------------------------------------------------------------------

int s_get(lua_State* L) {
  StoryRuntime* r = rt(L);
  const char* k = luaL_checkstring(L, 1);
  if (!r || !r->vars.has(k)) {
    if (lua_gettop(L) >= 2) lua_pushvalue(L, 2);
    else lua_pushnil(L);
    return 1;
  }
  push_var(L, r->vars.get(k));
  return 1;
}

int s_set(lua_State* L) {
  StoryRuntime* r = rt(L);
  const char* k = luaL_checkstring(L, 1);
  VarValue v;
  if (lua_isnoneornil(L, 2)) {
    if (r) r->vars.erase(k);
    return 0;
  }
  if (!to_var(L, 2, &v)) return luaL_argerror(L, 2, "boolean, number or string expected");
  if (r) {
    r->vars.set(k, v);
    r->dialogue_changed();
  }
  return 0;
}

int s_add(lua_State* L) {
  StoryRuntime* r = rt(L);
  const char* k = luaL_checkstring(L, 1);
  const double n = luaL_optnumber(L, 2, 1.0);
  lua_pushnumber(L, r ? r->vars.add(k, n) : 0.0);
  return 1;
}

int s_format(lua_State* L) {
  StoryRuntime* r = rt(L);
  const char* t = luaL_checkstring(L, 1);
  const std::string s = r ? r->vars.format(t) : std::string(t);
  lua_pushlstring(L, s.data(), s.size());
  return 1;
}

int s_check(lua_State* L) {
  StoryRuntime* r = rt(L);
  const char* c = luaL_checkstring(L, 1);
  if (!r) return lua_pushboolean(L, 0), 1;
  ConditionEnv env = r->dialogue.env;
  env.vars = &r->vars;
  if (!env.quest_status && r->features().quests) {
    env.quest_status = [r](const std::string& q) { return std::string(quest_status_name(r->quests.status(q))); };
    env.objective = [r](const std::string& q, const std::string& o) { return r->quests.objective_progress(q, o); };
  }
  std::string err;
  if (!check_condition(c, &err)) return luaL_error(L, "story.check: %s", err.c_str());
  lua_pushboolean(L, eval_condition(c, env));
  return 1;
}

int s_run(lua_State* L) {
  StoryRuntime* r = rt(L);
  const char* a = luaL_checkstring(L, 1);
  Action act;
  std::string err;
  if (!parse_action(a, &act, &err)) return ok_or_err(L, false, err);
  if (r) r->run_action(a);
  return ok_or_err(L, r != nullptr, "");
}

// ---- quests --------------------------------------------------------------------

int q_start(lua_State* L) {
  StoryRuntime* r = rt(L);
  std::string why;
  const bool ok = r && r->quests.start(luaL_checkstring(L, 1), &why);
  if (r) r->quests_changed();
  return ok_or_err(L, ok, why);
}

int q_progress(lua_State* L) {
  StoryRuntime* r = rt(L);
  const char* q = luaL_checkstring(L, 1);
  const char* o = luaL_checkstring(L, 2);
  const int n = static_cast<int>(luaL_optinteger(L, 3, 1));
  const bool ok = r && r->quests.progress(q, o, n);
  if (r) r->quests_changed();
  lua_pushboolean(L, ok);
  return 1;
}

int q_set(lua_State* L) {
  StoryRuntime* r = rt(L);
  const bool ok = r && r->quests.set_progress(luaL_checkstring(L, 1), luaL_checkstring(L, 2),
                                              static_cast<int>(luaL_checkinteger(L, 3)));
  if (r) r->quests_changed();
  lua_pushboolean(L, ok);
  return 1;
}

int q_complete(lua_State* L) {
  StoryRuntime* r = rt(L);
  const bool ok = r && r->quests.complete(luaL_checkstring(L, 1));
  if (r) r->quests_changed();
  lua_pushboolean(L, ok);
  return 1;
}

int q_fail(lua_State* L) {
  StoryRuntime* r = rt(L);
  const bool ok = r && r->quests.fail(luaL_checkstring(L, 1));
  if (r) r->quests_changed();
  lua_pushboolean(L, ok);
  return 1;
}

int q_status(lua_State* L) {
  StoryRuntime* r = rt(L);
  lua_pushstring(L, quest_status_name(r ? r->quests.status(luaL_checkstring(L, 1)) : QuestStatus::Inactive));
  return 1;
}

int q_count(lua_State* L) {
  StoryRuntime* r = rt(L);
  lua_pushinteger(L, r ? r->quests.objective_progress(luaL_checkstring(L, 1), luaL_checkstring(L, 2)) : 0);
  return 1;
}

int q_done(lua_State* L) {
  StoryRuntime* r = rt(L);
  const char* q = luaL_checkstring(L, 1);
  if (lua_isnoneornil(L, 2)) {
    lua_pushboolean(L, r && r->quests.status(q) == QuestStatus::Completed);
  } else {
    lua_pushboolean(L, r && r->quests.objective_done(q, luaL_checkstring(L, 2)));
  }
  return 1;
}

int q_list(lua_State* L) {
  StoryRuntime* r = rt(L);
  const char* want = luaL_optstring(L, 1, "");
  lua_newtable(L);
  if (!r) return 1;
  int i = 1;
  for (const QuestDef* d : r->quests.defs()) {
    const std::string st = quest_status_name(r->quests.status(d->id));
    if (*want && st != want) continue;
    lua_pushstring(L, d->id.c_str());
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

int q_info(lua_State* L) {
  StoryRuntime* r = rt(L);
  const QuestDef* d = r ? r->quests.def(luaL_checkstring(L, 1)) : nullptr;
  if (!d) return lua_pushnil(L), 1;
  lua_newtable(L);
  lua_pushstring(L, d->title.c_str());
  lua_setfield(L, -2, "title");
  lua_pushstring(L, d->description.c_str());
  lua_setfield(L, -2, "description");
  lua_pushstring(L, d->giver.c_str());
  lua_setfield(L, -2, "giver");
  lua_pushstring(L, quest_status_name(r->quests.status(d->id)));
  lua_setfield(L, -2, "status");
  lua_newtable(L);
  int i = 1;
  for (const ObjectiveDef& o : d->objectives) {
    lua_newtable(L);
    lua_pushstring(L, o.id.c_str());
    lua_setfield(L, -2, "id");
    lua_pushstring(L, o.text.c_str());
    lua_setfield(L, -2, "text");
    lua_pushinteger(L, o.count);
    lua_setfield(L, -2, "count");
    lua_pushinteger(L, r->quests.objective_progress(d->id, o.id));
    lua_setfield(L, -2, "progress");
    lua_pushboolean(L, o.optional);
    lua_setfield(L, -2, "optional");
    lua_rawseti(L, -2, i++);
  }
  lua_setfield(L, -2, "objectives");
  return 1;
}

int q_track(lua_State* L) {
  StoryRuntime* r = rt(L);
  if (r && !lua_isnoneornil(L, 1)) {
    r->quests.track(luaL_checkstring(L, 1));
    r->quests_changed();
  }
  lua_pushstring(L, r ? r->quests.tracked().c_str() : "");
  return 1;
}

int q_journal(lua_State* L) {
  StoryRuntime* r = rt(L);
  if (!r) return lua_pushboolean(L, 0), 1;
  const bool on = lua_isnoneornil(L, 1) ? !r->journal_open() : lua_toboolean(L, 1) != 0;
  r->show_journal(on);
  lua_pushboolean(L, on);
  return 1;
}

// ---- dialogue ------------------------------------------------------------------

int d_start(lua_State* L) {
  StoryRuntime* r = rt(L);
  std::string err = "no story ride";
  const bool ok = r && r->dialogue.start(luaL_checkstring(L, 1), luaL_optstring(L, 2, ""), &err);
  if (r) r->dialogue_changed();
  return ok_or_err(L, ok, ok ? "" : err);
}

int d_active(lua_State* L) {
  StoryRuntime* r = rt(L);
  if (r && r->dialogue.active()) {
    lua_pushstring(L, r->dialogue.dialogue_id().c_str());
  } else {
    lua_pushboolean(L, 0);
  }
  return 1;
}

int d_choose(lua_State* L) {
  StoryRuntime* r = rt(L);
  const bool ok = r && r->dialogue.choose(static_cast<int>(luaL_checkinteger(L, 1)) - 1);
  if (r) r->dialogue_changed();
  lua_pushboolean(L, ok);
  return 1;
}

int d_advance(lua_State* L) {
  StoryRuntime* r = rt(L);
  const bool ok = r && r->dialogue.advance();
  if (r) r->dialogue_changed();
  lua_pushboolean(L, ok);
  return 1;
}

int d_stop(lua_State* L) {
  StoryRuntime* r = rt(L);
  if (r) {
    r->dialogue.stop();
    r->dialogue_changed();
  }
  return 0;
}

int d_line(lua_State* L) {
  StoryRuntime* r = rt(L);
  if (!r || !r->dialogue.active()) return lua_pushnil(L), 1;
  const DialogueRunner::Line& l = r->dialogue.line();
  lua_newtable(L);
  lua_pushstring(L, l.node.c_str());
  lua_setfield(L, -2, "node");
  lua_pushstring(L, l.speaker.c_str());
  lua_setfield(L, -2, "speaker");
  lua_pushstring(L, l.text.c_str());
  lua_setfield(L, -2, "text");
  lua_pushstring(L, l.portrait.c_str());
  lua_setfield(L, -2, "portrait");
  lua_newtable(L);
  for (std::size_t i = 0; i < l.choices.size(); ++i) {
    lua_pushstring(L, l.choices[i].c_str());
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  lua_setfield(L, -2, "choices");
  return 1;
}

int d_event(lua_State* L) {
  StoryRuntime* r = rt(L);
  lua_pushstring(L, r ? r->last_event().c_str() : "");
  lua_pushstring(L, r ? r->last_event_arg().c_str() : "");
  return 2;
}

int d_list(lua_State* L) {
  StoryRuntime* r = rt(L);
  lua_newtable(L);
  if (!r) return 1;
  int i = 1;
  for (const std::string& id : r->dialogue.ids()) {
    lua_pushstring(L, id.c_str());
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

// ---- saves ---------------------------------------------------------------------

int v_save(lua_State* L) {
  StoryRuntime* r = rt(L);
  std::string err = "no story ride";
  const bool ok = r && r->save(luaL_checkstring(L, 1), luaL_optstring(L, 2, ""), &err);
  return ok_or_err(L, ok, ok ? "" : err);
}

int v_load(lua_State* L) {
  StoryRuntime* r = rt(L);
  std::string err = "no story ride";
  const bool ok = r && r->load(luaL_checkstring(L, 1), &err);
  return ok_or_err(L, ok, ok ? "" : err);
}

int v_autosave(lua_State* L) {
  StoryRuntime* r = rt(L);
  std::string err = "no story ride", slot;
  const bool ok = r && r->autosave(&slot, &err);
  if (ok) {
    lua_pushstring(L, slot.c_str());
    return 1;
  }
  return ok_or_err(L, false, err);
}

int v_exists(lua_State* L) {
  StoryRuntime* r = rt(L);
  lua_pushboolean(L, r && r->saves.exists(luaL_checkstring(L, 1)));
  return 1;
}

int v_delete(lua_State* L) {
  StoryRuntime* r = rt(L);
  lua_pushboolean(L, r && r->saves.remove(luaL_checkstring(L, 1)));
  return 1;
}

int v_latest(lua_State* L) {
  StoryRuntime* r = rt(L);
  const std::string s = r ? r->saves.latest() : std::string();
  if (s.empty()) return lua_pushnil(L), 1;
  lua_pushstring(L, s.c_str());
  return 1;
}

int v_list(lua_State* L) {
  StoryRuntime* r = rt(L);
  lua_newtable(L);
  if (!r) return 1;
  int i = 1;
  for (const SlotInfo& s : r->saves.list()) {
    lua_newtable(L);
    lua_pushstring(L, s.slot.c_str());
    lua_setfield(L, -2, "slot");
    lua_pushstring(L, s.label.c_str());
    lua_setfield(L, -2, "label");
    lua_pushstring(L, s.time.c_str());
    lua_setfield(L, -2, "time");
    lua_pushnumber(L, s.playtime);
    lua_setfield(L, -2, "playtime");
    lua_pushboolean(L, s.autosave);
    lua_setfield(L, -2, "auto");
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

int v_menu(lua_State* L) {
  StoryRuntime* r = rt(L);
  if (!r) return lua_pushboolean(L, 0), 1;
  const bool on = lua_isnoneornil(L, 1) ? !r->save_menu_open() : lua_toboolean(L, 1) != 0;
  r->show_save_menu(on);
  lua_pushboolean(L, on);
  return 1;
}

int v_autosave_every(lua_State* L) {
  StoryRuntime* r = rt(L);
  if (r) r->settings.autosave_seconds = std::max(0.0, static_cast<double>(luaL_checknumber(L, 1)));
  return 0;
}

int v_playtime(lua_State* L) {
  StoryRuntime* r = rt(L);
  lua_pushnumber(L, r ? r->playtime() : 0.0);
  return 1;
}

}  // namespace

void push_story_lua(lua_State* L, StoryRuntime** slot) {
  const luaL_Reg fns[] = {{"get", &s_get},       {"set", &s_set},   {"add", &s_add},
                          {"format", &s_format}, {"check", &s_check}, {"run", &s_run},
                          {nullptr, nullptr}};
  set_fns(L, slot, fns);
}

void push_quests_lua(lua_State* L, StoryRuntime** slot) {
  const luaL_Reg fns[] = {{"start", &q_start},     {"progress", &q_progress}, {"set", &q_set},
                          {"complete", &q_complete}, {"fail", &q_fail},       {"status", &q_status},
                          {"count", &q_count},     {"done", &q_done},         {"list", &q_list},
                          {"info", &q_info},       {"track", &q_track},       {"journal", &q_journal},
                          {nullptr, nullptr}};
  set_fns(L, slot, fns);
}

void push_dialogue_lua(lua_State* L, StoryRuntime** slot) {
  const luaL_Reg fns[] = {{"start", &d_start},   {"active", &d_active}, {"choose", &d_choose},
                          {"advance", &d_advance}, {"stop", &d_stop},   {"line", &d_line},
                          {"last_event", &d_event}, {"list", &d_list},  {nullptr, nullptr}};
  set_fns(L, slot, fns);
}

void push_saves_lua(lua_State* L, StoryRuntime** slot) {
  const luaL_Reg fns[] = {{"save", &v_save},         {"load", &v_load},     {"autosave", &v_autosave},
                          {"exists", &v_exists},     {"delete", &v_delete}, {"latest", &v_latest},
                          {"list", &v_list},         {"menu", &v_menu},     {"autosave_every", &v_autosave_every},
                          {"playtime", &v_playtime}, {nullptr, nullptr}};
  set_fns(L, slot, fns);
}

}  // namespace story
}  // namespace tombstone
}  // namespace ts
