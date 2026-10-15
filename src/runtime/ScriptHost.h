#pragma once

// ScriptHost: the language-neutral face of gameplay scripting. The world
// (2D World, World3D, the server's World) drives one ScriptHost; each
// entity's script component is routed to the host of its file's language
// (script_language): Lua (LuaScriptHost), visual graphs (GraphScriptHost),
// C++ modules (NativeScriptHost) and C# (CSharpScriptHost). Every language
// gets the same hooks -- on_start, on_tick, on_trigger_enter/exit,
// on_interact, on_reload, the VR hand events, UI events, destroy (detach) --
// and the same ScriptWorld API. Languages are created on first use, and
// every phase walks them in a fixed order (lua, graph, cpp, cs) so a mixed
// project ticks deterministically.

#include "runtime/Script.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace ts {
namespace tombstone {
namespace runtime {

using ScriptLanguageFactory = std::function<std::unique_ptr<ScriptLanguageHost>(
    ScriptWorld& world, ScriptLibrary& library, const std::string& project_dir)>;

// Plugs a language in (by script_language id). Built-ins register
// themselves; a later registration replaces the earlier one.
void register_script_language(const std::string& id, ScriptLanguageFactory factory);

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

  bool attach(const Entity2D& e);
  void detach(std::uint64_t entity);
  bool has(std::uint64_t entity) const;
  bool running(std::uint64_t entity) const;
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
  int reload_changed();
  bool reload(const std::string& rel);

  // Language host by id ("lua", "graph", "cpp", "cs"); null when that
  // language has not been used (or is unknown).
  ScriptLanguageHost* language(const std::string& id) const;
  LuaScriptHost& lua() { return *lua_; }

  // Props a script declares, by language (Lua: `props = {...}`; graphs:
  // their "props" object; C++ / C#: none at edit time).
  static bool describe(const std::string& source, const std::string& rel,
                       std::vector<ScriptProp>* defaults, std::string* error);

 private:
  ScriptLanguageHost* host_for(const std::string& lang);
  ScriptLanguageHost* owner(std::uint64_t entity) const;

  ScriptWorld& world_;
  ScriptLibrary& library_;
  std::string project_dir_;
  LuaScriptHost* lua_ = nullptr;
  std::array<std::unique_ptr<ScriptLanguageHost>, kLanguages> hosts_;
  std::unordered_map<std::uint64_t, std::size_t> owner_;
};

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
