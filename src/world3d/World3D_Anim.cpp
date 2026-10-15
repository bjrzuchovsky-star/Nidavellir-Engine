// Skeletal animation in the 3D runtime: one AnimatorRuntime per entity with
// an `animator`, the rider controller's parameters, clip events into
// scripts (on_anim_event), root motion, VR skinned hands, and the Lua
// `anim` table (graphs reach it through the plugin call bridge).

#include "world3d/World3D.h"

#include "render3d/Gltf.h"
#include "runtime/ScriptState.h"

#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

namespace ts {
namespace tombstone {
namespace world3d {

namespace fs = std::filesystem;
using render3d::AnimatorRuntime;
using render3d::Mat4;
using runtime::LogLevel;

struct World3D::AnimSystem {
  struct Slot {
    AnimatorRuntime rt;
    bool ok = false;
  };
  std::map<std::string, std::shared_ptr<render3d::Model>> models;  // null = failed
  std::map<std::uint64_t, Slot> slots;
  std::set<std::string> reported;
  bool was_grounded = true;
  std::shared_ptr<const std::vector<Mat4>> hands[2];
};

void World3D::AnimSystemDeleter::operator()(AnimSystem* a) const { delete a; }

namespace {

std::shared_ptr<render3d::Model> load_model(World3D::AnimSystem* a, const std::string& dir,
                                            const std::string& rel, std::string* why) {
  auto it = a->models.find(rel);
  if (it != a->models.end()) return it->second;
  auto m = std::make_shared<render3d::Model>();
  std::string err;
  if (!render3d::load_gltf((fs::path(dir) / fs::path(rel)).string(), m.get(), &err) ||
      !m->skinned()) {
    if (why) *why = err.empty() ? rel + " has no skin" : err;
    m.reset();
  }
  a->models[rel] = m;
  return m;
}

}  // namespace

AnimatorRuntime* World3D::animator(std::uint64_t id) {
  Entity3D* e = live(id);
  if (!e || !e->animator) return nullptr;
  if (!anim_) anim_.reset(new AnimSystem());
  auto it = anim_->slots.find(id);
  if (it == anim_->slots.end()) {
    AnimSystem::Slot& s = anim_->slots[id];
    std::string rel = e->animator->model;
    if (rel.empty() && e->mesh && !e->mesh->is_primitive()) rel = e->mesh->source;
    std::string why;
    std::shared_ptr<render3d::Model> m = rel.empty() ? nullptr : load_model(anim_.get(), project_dir_, rel, &why);
    if (!m) {
      if (anim_->reported.insert("model:" + std::to_string(id)).second) {
        post(LogLevel::Warn, "anim",
             e->name + ": animator has no skinned model" + (why.empty() ? "" : " (" + why + ")"), id);
      }
      return nullptr;
    }
    std::string problems;
    s.ok = s.rt.bind(*e->animator, m.get(), &problems);
    if (!problems.empty() && anim_->reported.insert("clips:" + rel).second) {
      post(LogLevel::Warn, "anim", e->name + ": " + problems, id);
    }
    it = anim_->slots.find(id);
  }
  return it->second.ok ? &it->second.rt : nullptr;
}

std::size_t World3D::animators() const {
  if (!anim_) return 0;
  std::size_t n = 0;
  for (const auto& [id, s] : anim_->slots) n += s.ok ? 1 : 0;
  return n;
}

bool World3D::anim_play(std::uint64_t id, const std::string& name, float fade, bool hold) {
  AnimatorRuntime* rt = animator(id);
  return rt && rt->play(name, fade, hold);
}

std::shared_ptr<const std::vector<Mat4>> World3D::vr_hand_palette(int hand) const {
  if (!anim_) return nullptr;
  return anim_->hands[hand & 1];
}

void World3D::anim_step() {
  const float dt = static_cast<float>(kTickSeconds);
  bool any = false;
  for (const Entity3D& e : ents_) {
    if (e.animator && !dead_.count(e.id)) any = true;
  }
  if (!any && !(vr_ && !rig_cfg_.hand_model.empty())) {
    if (anim_) anim_->slots.clear();
    return;
  }
  if (!anim_) anim_.reset(new AnimSystem());
  // Forget entities that left.
  for (auto it = anim_->slots.begin(); it != anim_->slots.end();) {
    const Entity3D* e = find(it->first);
    it = (!e || !e->animator || dead_.count(it->first)) ? anim_->slots.erase(it) : std::next(it);
  }
  const bool takeoff = anim_->was_grounded && !grounded_ && vel_.y > 0.5f;
  anim_->was_grounded = grounded_;
  for (std::size_t i = 0; i < ents_.size(); ++i) {
    if (!ents_[i].animator || dead_.count(ents_[i].id)) continue;
    const std::uint64_t id = ents_[i].id;
    AnimatorRuntime* rt = animator(id);
    if (!rt) continue;
    Entity3D& e = ents_[i];
    if (id == player_ && !vr_ && e.animator->drive_from_player) {
      rt->set_float("speed", std::sqrt(vel_.x * vel_.x + vel_.z * vel_.z));
      rt->set_float("vertical_speed", vel_.y);
      if (rt->has_param("grounded")) rt->set_bool("grounded", grounded_);
      if (takeoff && rt->has_param("jump")) rt->trigger("jump");
    }
    rt->update(dt);
    for (const AnimatorRuntime::Fired& f : rt->take_events()) {
      if (host_ && host_->running(id) && host_->has_hook(id, "on_anim_event")) {
        host_->hand_event(id, "on_anim_event", f.name, f.time);
      }
    }
    const render3d::Vec3 rm = rt->take_root_motion();
    if (e.animator->root_motion && id != player_ && (rm.x != 0.0f || rm.z != 0.0f)) {
      e.transform.position += e.transform.rotation.rotate(render3d::mul(rm, e.transform.scale));
    }
    auto pal = std::make_shared<std::vector<Mat4>>();
    rt->palette(pal.get());
    e.skin_palette = std::move(pal);
  }
  // VR hands: the hand model's "grip" clip follows the squeeze.
  anim_->hands[0].reset();
  anim_->hands[1].reset();
  if (vr_ && !rig_cfg_.hand_model.empty()) {
    std::string why;
    std::shared_ptr<render3d::Model> m = load_model(anim_.get(), project_dir_, rig_cfg_.hand_model, &why);
    if (!m) {
      if (anim_->reported.insert("hand").second) {
        post(LogLevel::Warn, "vr", "hand_model " + rig_cfg_.hand_model + ": " + why);
      }
      return;
    }
    const int grip = m->find_clip("grip");
    for (int h = 0; h < 2; ++h) {
      render3d::Pose pose = render3d::rest_pose(m->skeleton);
      if (grip >= 0) {
        const render3d::Clip& c = m->clips[static_cast<std::size_t>(grip)];
        const float k = std::clamp(std::max(hands_[h].squeeze, hands_[h].held ? 1.0f : 0.0f), 0.0f, 1.0f);
        render3d::sample_clip(c, k * c.duration, false, &pose);
      }
      auto pal = std::make_shared<std::vector<Mat4>>();
      render3d::skin_palette(m->skeleton, pose, pal.get());
      anim_->hands[h] = std::move(pal);
    }
  }
}

// ---- Lua: the anim table ---------------------------------------------------------

namespace {

constexpr const char* kEntityMeta = "TombStone.Entity";  // see ScriptApi.cpp

World3D& world(lua_State* L) {
  return *static_cast<World3D*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// An entity handle, an id number, or a name.
std::uint64_t entity_arg(lua_State* L, int idx) {
  if (const auto* id = static_cast<std::uint64_t*>(luaL_testudata(L, idx, kEntityMeta))) return *id;
  if (lua_type(L, idx) == LUA_TNUMBER) {
    const lua_Number n = lua_tonumber(L, idx);
    return n > 0 && n < 9e15 ? static_cast<std::uint64_t>(n) : 0;
  }
  if (lua_type(L, idx) == LUA_TSTRING) return world(L).script_find(lua_tostring(L, idx));
  return luaL_argerror(L, idx, "entity expected"), 0;
}

float fade_arg(lua_State* L, int idx) {
  if (lua_isnoneornil(L, idx)) return -1.0f;
  const lua_Number n = luaL_checknumber(L, idx);
  return std::isfinite(n) ? static_cast<float>(std::clamp<lua_Number>(n, 0, 10)) : -1.0f;
}

// anim.play(e, state_or_clip [, fade [, hold]]) -> bool
int lua_anim_play(lua_State* L) {
  const std::uint64_t id = entity_arg(L, 1);
  const char* name = luaL_checkstring(L, 2);
  lua_pushboolean(L, world(L).anim_play(id, name, fade_arg(L, 3), lua_toboolean(L, 4) != 0));
  return 1;
}

// anim.crossfade(e, state_or_clip, seconds) -> bool
int lua_anim_crossfade(lua_State* L) {
  const std::uint64_t id = entity_arg(L, 1);
  const char* name = luaL_checkstring(L, 2);
  const float fade = lua_isnoneornil(L, 3) ? 0.25f : fade_arg(L, 3);
  lua_pushboolean(L, world(L).anim_play(id, name, fade, false));
  return 1;
}

// anim.set_param(e, name, number | bool)
int lua_anim_set_param(lua_State* L) {
  AnimatorRuntime* rt = world(L).animator(entity_arg(L, 1));
  const char* name = luaL_checkstring(L, 2);
  if (!rt) return 0;
  if (lua_type(L, 3) == LUA_TBOOLEAN) {
    rt->set_bool(name, lua_toboolean(L, 3) != 0);
  } else {
    const lua_Number n = luaL_checknumber(L, 3);
    if (std::isfinite(n)) rt->set_param(name, static_cast<float>(n));
  }
  return 0;
}

int lua_anim_param(lua_State* L) {
  AnimatorRuntime* rt = world(L).animator(entity_arg(L, 1));
  const char* name = luaL_checkstring(L, 2);
  lua_pushnumber(L, rt ? rt->param(name) : 0.0);
  return 1;
}

int lua_anim_trigger(lua_State* L) {
  AnimatorRuntime* rt = world(L).animator(entity_arg(L, 1));
  const char* name = luaL_checkstring(L, 2);
  if (rt) rt->trigger(name);
  return 0;
}

// anim.state(e [, layer = 1]) -> state name ("" when none)
int lua_anim_state(lua_State* L) {
  AnimatorRuntime* rt = world(L).animator(entity_arg(L, 1));
  const int layer = static_cast<int>(luaL_optinteger(L, 2, 1)) - 1;
  lua_pushstring(L, rt ? rt->state(layer).c_str() : "");
  return 1;
}

// anim.stop(e, layer [, fade])
int lua_anim_stop(lua_State* L) {
  AnimatorRuntime* rt = world(L).animator(entity_arg(L, 1));
  const int layer = static_cast<int>(luaL_optinteger(L, 2, 2)) - 1;
  const float fade = lua_isnoneornil(L, 3) ? 0.2f : fade_arg(L, 3);
  if (rt) rt->stop_layer(layer, fade);
  return 0;
}

}  // namespace

void World3D::anim_open_lua(lua_State* L, int sandbox) {
  const luaL_Reg fns[] = {{"play", lua_anim_play},       {"crossfade", lua_anim_crossfade},
                          {"set_param", lua_anim_set_param}, {"param", lua_anim_param},
                          {"trigger", lua_anim_trigger}, {"state", lua_anim_state},
                          {"stop", lua_anim_stop},       {nullptr, nullptr}};
  lua_newtable(L);
  lua_pushlightuserdata(L, this);
  luaL_setfuncs(L, fns, 1);
  lua_setfield(L, sandbox, "anim");
}

}  // namespace world3d
}  // namespace tombstone
}  // namespace ts
