#pragma once

// What plugins bolt onto a World: extra systems that run every fixed tick
// (after scripts, before animation), Lua tables added to every script's
// sandbox, the component names they claim, draw layers (quads under / over
// the actors), blockers (solid boxes riders and bodies collide with) and
// network channels (messages between a server's and its clients' worlds). Plain data built by
// ts_plugin's PluginManager; the runtime only consumes it, so ts_runtime
// never depends on the plugin layer.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct lua_State;

namespace ts {
namespace tombstone {
namespace runtime {

class World;
struct DrawQuad;
struct WorldRect;
struct Aabb;

struct WorldExtensions {
  struct System {
    std::string name;   // "tumbleweed.drift"
    std::string owner;  // plugin id, for errors in the Telegraph
    std::function<void(World& world, double dt)> run;
  };
  struct LuaApi {
    std::string name;   // global the script sees ("tumbleweed")
    std::string owner;
    // Push exactly one value (usually a table); it lands in the sandbox
    // under `name`. Must leave the stack balanced apart from that value.
    std::function<void(lua_State* L)> push;
  };
  struct Component {
    std::string name;   // "Tumbleweed"
    std::string owner;
    std::string description;
  };

  struct DrawLayer {
    std::string name;
    std::string owner;
    bool over = false;  // false: under the actors; true: on top of them
    // Append quads (world space) for `view`; alpha = tick interpolation.
    std::function<void(const World& world, const WorldRect& view, float alpha,
                       std::vector<DrawQuad>* out)>
        draw;
  };
  struct Blocker {
    std::string name;
    std::string owner;
    // Append the solid boxes overlapping `area` (world space).
    std::function<void(const World& world, const Aabb& area, std::vector<Aabb>* out)> solids;
  };
  struct NetChannel {
    std::string name;   // "frontier"
    std::string owner;
    // A message for this channel arrived. On a server from_slot is the
    // sender's rider slot; on a client it is -1 (from the server).
    std::function<void(World& world, int from_slot, const std::vector<std::uint8_t>& bytes)>
        receive;
  };

  std::vector<System> systems;      // run in plugin load order
  std::vector<LuaApi> lua;
  std::vector<Component> components;
  std::vector<DrawLayer> layers;
  std::vector<Blocker> blockers;
  std::vector<NetChannel> channels;

  bool empty() const {
    return systems.empty() && lua.empty() && components.empty() && layers.empty() &&
           blockers.empty() && channels.empty();
  }
};

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
