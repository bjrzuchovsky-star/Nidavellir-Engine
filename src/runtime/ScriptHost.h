#pragma once

// ScriptHost: the language-neutral face of gameplay scripting. The world
// (2D World, World3D, the server's World) drives one ScriptHost; each
// entity's script component is routed to the host of its file's language
// (script_language): Lua (LuaScriptHost), visual graphs (GraphScriptHost),
// C++ modules (NativeScriptHost) and C# (CSharpScriptHost). Every language
// gets the same hooks -- on_start, on_tick, on_trigger_enter/exit,
// on_interact, on_reload, the VR hand events, UI events, destroy (detach) --
// and the same ScriptWorld API. Languages are created on first use.
//
// An entity may carry several scripts (any mix of languages; see
// script_stack in SceneData.h). Order is the Inspector's, top to bottom:
//   - per-object events (triggers, on_interact, VR hand events, impacts,
//     anim events) run that object's scripts in its stack order;
//   - world-wide phases (on_start, on_tick, plugin broadcasts) run stack
//     position by position: every object's first script (languages in the
//     fixed order lua, graph, cpp, cs), then every object's second script,
//     and so on. A one-script scene therefore ticks exactly as before, and
//     an object's scripts never run out of their Inspector order.
// Disabled scripts (ScriptData::enabled false) are not attached at all.

#include "runtime/Script.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

using ScriptLanguageFactory = std::function<std::unique_ptr<ScriptLanguageHost>(
    ScriptWorld& world, ScriptLibrary& library, const std::string& project_dir)>;

// Plugs a language in (by script_language id). Built-ins register
// themselves; a later registration replaces the earlier one.
void register_script_language(const std::string& id, ScriptLanguageFactory factory);

// C++ (`prop("reach", 24.0)`) / C# (`Prop("reach", 24.0)`) props with a
// literal default, found by scanning the source (sorted by name).
std::vector<ScriptProp> describe_compiled_props(const std::string& source, bool csharp);

class ScriptHost {
 public:
  static constexpr std::size_t kMemoryLimit = LuaScriptHost::kMemoryLimit;
  static constexpr long long kInstructionBudget = LuaScriptHost::kInstructionBudget;
  static constexpr std::size_t kMaxTimers = LuaScriptHost::kMaxTimers;
  static constexpr std::size_t kMaxStateKeys = LuaScriptHost::kMaxStateKeys;
  static constexpr std::size_t kLanguages = 4;  // lua, graph, cpp, cs

  ScriptHost(ScriptWorld& world, ScriptLibrary& library, std::string project_dir);
  ~ScriptHost();
  ScriptHost(const ScriptHost&) = delete;
  ScriptHost& operator=(const ScriptHost&) = delete;

  // Attach every enabled script on `e` (its whole stack). True when at
  // least one attached and none failed to load.
  bool attach(const Entity2D& e);
  void detach(std::uint64_t entity);
  bool has(std::uint64_t entity) const;
  // Any of the entity's scripts loaded and not stopped.
  bool running(std::uint64_t entity) const;
  // The script at stack position `index` (0 = first; disabled ones are not
  // running).
  bool running(std::uint64_t entity, int index) const;
  // Number of scripts attached to the entity (enabled ones).
  std::size_t script_count(std::uint64_t entity) const;
  // Some running script on the entity defines the hook.
  bool has_hook(std::uint64_t entity, const char* hook);
  std::size_t instance_count() const;
  std::size_t timer_count() const;     // Lua timers
  std::size_t memory_used() const;     // Lua heap
  std::size_t ui_handler_count() const;

  void start_pending();
  void trigger(bool enter, std::uint64_t trigger, std::uint64_t other);
  void interact(std::uint64_t target, std::uint64_t rider);
  void ui_event(const std::string& widget, bool change, double value, const std::string& text);
  void hand_event(std::uint64_t target, const char* hook, const std::string& text,
                  double number);
  void run_timers(std::uint64_t tick);
  // hook(text, number) on every running script that has it, every language.
  void broadcast(const char* hook, const std::string& text, double number);
  void tick(double dt);
  void fixed_tick(double dt);
  void late_tick(double dt);
  int reload_changed();
  bool reload(const std::string& rel);

  // Language host by id ("lua", "graph", "cpp", "cs"); null when that
  // language has not been used (or is unknown).
  ScriptLanguageHost* language(const std::string& id) const;
  LuaScriptHost& lua() { return *lua_; }

  // Props a script declares, by language (Lua: `props = {...}`; graphs:
  // their "props" object; C++ / C#: prop("name", literal) / Prop(...) calls
  // found in the source, see describe_compiled_props).
  static bool describe(const std::string& source, const std::string& rel,
                       std::vector<ScriptProp>* defaults, std::string* error);

 private:
  ScriptLanguageHost* host_for(const std::string& lang);
  struct Slot {
    std::size_t lang = 0;  // index into hosts_
    int index = 0;         // stack position (slot id the language host sees)
  };
  const std::vector<Slot>* slots(std::uint64_t entity) const;

  ScriptWorld& world_;
  ScriptLibrary& library_;
  std::string project_dir_;
  LuaScriptHost* lua_ = nullptr;
  std::array<std::unique_ptr<ScriptLanguageHost>, kLanguages> hosts_;
  std::unordered_map<std::uint64_t, std::vector<Slot>> owner_;
  int depth_ = 1;  // stack positions in use (world-wide phases run 0 .. depth_-1)
};

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
