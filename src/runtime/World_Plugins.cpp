// World hooks for plugins: extension systems on the fixed tick and Lua
// globals in every script sandbox (see WorldExtensions.h).

#include "runtime/ScriptState.h"
#include "runtime/World.h"

#include <exception>
#include <string>
#include <utility>

namespace ts {
namespace tombstone {
namespace runtime {

namespace {

using PushFn = std::function<void(lua_State*)>;

// Runs a plugin's push inside lua_pcall so a Lua error (or a C++ throw,
// turned into one) cannot unwind through the script host.
int push_trampoline(lua_State* L) {
  const auto* fn = static_cast<const PushFn*>(lua_touserdata(L, 1));
  lua_settop(L, 0);
  std::string error;
  try {
    (*fn)(L);
  } catch (const std::exception& e) {
    error = e.what();
  } catch (...) {
    error = "unknown C++ exception";
  }
  if (!error.empty()) {
    lua_settop(L, 0);
    lua_pushstring(L, error.c_str());
    return lua_error(L);
  }
  return lua_gettop(L) >= 1 ? 1 : 0;
}

}  // namespace

void World::set_extensions(std::shared_ptr<const WorldExtensions> ext) {
  ext_ = std::move(ext);
  ext_dead_.assign(ext_ ? ext_->systems.size() : 0, false);
}

std::size_t World::extension_systems_live() const {
  if (!ext_) return 0;
  std::size_t n = 0;
  for (std::size_t i = 0; i < ext_->systems.size(); ++i) {
    if (i >= ext_dead_.size() || !ext_dead_[i]) ++n;
  }
  return n;
}

void World::script_open_extensions(lua_State* L, int sandbox) {
  if (!ext_) return;
  for (const WorldExtensions::LuaApi& api : ext_->lua) {
    if (api.name.empty() || !api.push) continue;
    lua_pushcfunction(L, &push_trampoline);
    lua_pushlightuserdata(L, const_cast<PushFn*>(&api.push));
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
      const char* msg = lua_tostring(L, -1);
      post(LogLevel::Error, "plugin",
           "[" + api.owner + "] Lua table '" + api.name +
               "' failed to load: " + (msg ? msg : "?"),
           0);
      lua_pop(L, 1);
      continue;
    }
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      continue;
    }
    lua_setfield(L, sandbox, api.name.c_str());
  }
}

void World::net_post(std::string channel, int to_slot, std::vector<std::uint8_t> bytes) {
  if (net_role_ == NetRole::Local) return;
  if (net_posts_.size() >= kMaxNetPosts) net_posts_.erase(net_posts_.begin());
  net_posts_.push_back(NetPost{std::move(channel), to_slot, std::move(bytes)});
}

std::vector<NetPost> World::take_net_posts() {
  std::vector<NetPost> out;
  out.swap(net_posts_);
  return out;
}

void World::play_sound(SoundEvent event) {
  ++sound_total_;
  if (net_role_ == NetRole::Server) {
    const int to = event.slot;
    net_post(kSoundChannel, to, encode_sound(event));
  }
  if (sounds_.size() >= kMaxSounds) sounds_.erase(sounds_.begin());
  sounds_.push_back(std::move(event));
}

std::vector<SoundEvent> World::take_sounds() {
  std::vector<SoundEvent> out;
  out.swap(sounds_);
  return out;
}

void World::script_sound(SoundEvent event) { play_sound(std::move(event)); }

bool World::net_deliver(const std::string& channel, int from_slot,
                        const std::vector<std::uint8_t>& bytes) {
  if (channel == kSoundChannel) {
    // Only the server speaks on it; a rider's client cannot make others
    // hear things.
    SoundEvent ev;
    if (net_role_ != NetRole::Client || !decode_sound(bytes, &ev)) return false;
    ev.slot = -1;
    if (sounds_.size() >= kMaxSounds) sounds_.erase(sounds_.begin());
    sounds_.push_back(std::move(ev));
    ++sound_total_;
    return true;
  }
  if (!ext_) return false;
  for (const WorldExtensions::NetChannel& ch : ext_->channels) {
    if (ch.name != channel || !ch.receive) continue;
    try {
      ch.receive(*this, from_slot, bytes);
      return true;
    } catch (const std::exception& e) {
      post(LogLevel::Error, "plugin",
           "[" + ch.owner + "] channel '" + ch.name + "' threw: " + e.what(), 0);
    } catch (...) {
      post(LogLevel::Error, "plugin", "[" + ch.owner + "] channel '" + ch.name + "' threw", 0);
    }
    return false;
  }
  return false;
}

void World::step_extensions() {
  if (!ext_ || ext_->systems.empty()) return;
  if (ext_dead_.size() != ext_->systems.size()) {
    ext_dead_.assign(ext_->systems.size(), false);
  }
  for (std::size_t i = 0; i < ext_->systems.size(); ++i) {
    const WorldExtensions::System& sys = ext_->systems[i];
    if (ext_dead_[i] || !sys.run) continue;
    try {
      sys.run(*this, kTickSeconds);
    } catch (const std::exception& e) {
      ext_dead_[i] = true;
      post(LogLevel::Error, "plugin",
           "[" + sys.owner + "] system '" + sys.name +
               "' threw and is off for this ride: " + e.what(),
           0);
    } catch (...) {
      ext_dead_[i] = true;
      post(LogLevel::Error, "plugin",
           "[" + sys.owner + "] system '" + sys.name +
               "' threw and is off for this ride", 0);
    }
  }
}

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
