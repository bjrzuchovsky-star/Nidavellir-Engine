#pragma once

// What plugins bolt onto a World: extra systems that run every fixed tick
// (after scripts, before animation), Lua tables added to every script's
// sandbox, and the component names they claim. Plain data built by
// ts_plugin's PluginManager; the runtime only consumes it, so ts_runtime
// never depends on the plugin layer.

#include <functional>
#include <string>
#include <vector>

struct lua_State;

namespace ts {
namespace tombstone {
namespace runtime {

class World;

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

  std::vector<System> systems;      // run in plugin load order
  std::vector<LuaApi> lua;
  std::vector<Component> components;

  bool empty() const {
    return systems.empty() && lua.empty() && components.empty();
  }
};

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
