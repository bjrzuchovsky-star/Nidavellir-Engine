#pragma once

// Gameplay scripts: sandboxed Lua 5.4, run by the world on its fixed tick.
//
// Each script on an entity (an entity may carry several, see script_stack
// in SceneData.h) gets its own instance of the file (own globals, own
// `props`; `self` is the entity, shared by its scripts). Hooks the file may define:
//   on_start()                  first tick the entity is in the world
//   on_tick(dt)                 every tick (dt = 1/60 s)
//   on_trigger_enter(other)     somebody rode into this entity's trigger
//   on_trigger_exit(other)      ...and back out
//   on_interact(rider)          a rider pressed the action button close by
//   on_reload()                 the file was hot-reloaded during Play
// Scripts reach the world only through ScriptWorld (World implements it):
// no files, no OS, no clock, no randomness the server cannot replay. A
// script that errors is logged with file:line and stopped; the rest of the
// world keeps riding.

#include "runtime/Log.h"
#include "runtime/Sound.h"
#include "scene/SceneData.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace ts {
namespace tombstone {
namespace runtime {

// A script's request to the in-game UI (Lua ui.*): queued by the world and
// applied by the host's ui::UiSystem (see ui/UiScript.h).
struct UiCommand {
  // AddDocument: text is a *.ui.json document, added unless the host
  // already has one named `target` (a project's own ui/<name>.ui.json
  // wins). SetItems: a list widget's items, '\n'-separated in text.
  // Focus: target is "doc.widget".
  enum class Kind { Show, Hide, SetText, SetValue, SetVisible, AddDocument, SetItems, Focus };
  Kind kind = Kind::Show;
  std::string target;  // document name, or widget id ("doc.widget" or "widget")
  std::string text;
  double value = 0.0;  // SetValue; SetVisible (0 / 1)
};

// Scripts tick with the world (World::kTickRate; checked where they meet).
inline constexpr int kScriptTickRate = 60;

// "scripts/gate.lua"-style: relative, forward slashes, no "." / ".."
// parts, ends in a script language's extension (see script_language).
bool valid_script_path(const std::string& rel);
// Language a script file is written in, by extension: "lua" (.lua),
// "graph" (.graph.json, visual scripts), "cpp" (.cpp, compiled into the
// project's native module), "cs" (.cs, .NET); "" for anything else.
std::string script_language(const std::string& rel);

// Script sources by project-relative path, re-read when the file changes on
// disk. put() pins an in-memory source (tests, unsaved buffers) that wins
// over the disk.
class ScriptLibrary {
 public:
  static constexpr std::size_t kMaxBytes = 1u << 20;

  struct Entry {
    std::string path;
    std::string text;
    bool ok = false;
    std::string error;          // why it could not be read
    std::int64_t stamp = 0;     // file time + size (changes on save)
    std::uint64_t version = 0;  // bumps whenever text / ok changes
    bool memory = false;        // pinned by put()
    bool dirty = false;         // changed since the last refresh()
  };

  // Cached entry, (re-)read from <project_dir>/<rel> when it changed.
  const Entry* get(const std::string& project_dir, const std::string& rel);
  void put(const std::string& rel, std::string text);
  void forget(const std::string& rel);
  void clear() { entries_.clear(); }
  // Re-check every cached file; returns the paths whose source changed.
  std::vector<std::string> refresh(const std::string& project_dir);

 private:
  Entry* slot(const std::string& rel);
  void read(const std::string& project_dir, Entry* e);
  std::vector<Entry> entries_;
};

// The world as scripts see it. Every call is safe on a missing id (no-op /
// null / false). Slots are 0-based here (scripts see 1-based).
class ScriptWorld {
 public:
  virtual ~ScriptWorld() = default;
  // Tick being simulated (1 = the first); 0 before the first tick.
  virtual std::uint64_t script_tick() const = 0;
  // Live entity (moves with the ride); null once destroyed.
  virtual const Entity2D* script_entity(std::uint64_t id) const = 0;
  virtual std::uint64_t script_find(const std::string& name) const = 0;
  virtual std::uint64_t script_player(int slot) const = 0;
  virtual void script_move(std::uint64_t id, float x, float y) = 0;
  virtual bool script_visible(std::uint64_t id) const = 0;
  virtual void script_set_visible(std::uint64_t id, bool on) = 0;
  virtual bool script_collider_on(std::uint64_t id) const = 0;
  virtual void script_set_collider(std::uint64_t id, bool on) = 0;
  virtual bool script_play(std::uint64_t id, const std::string& clip,
                           bool hold) = 0;
  virtual void script_release(std::uint64_t id) = 0;
  // New entity from an authored one (by name) / a copy of a live one.
  // 0 when there is no such entity or the world is full.
  virtual std::uint64_t script_spawn(const std::string& tmpl, float x,
                                     float y) = 0;
  virtual std::uint64_t script_duplicate(std::uint64_t id, float x,
                                         float y) = 0;
  virtual void script_destroy(std::uint64_t id) = 0;
  virtual bool script_spawned(std::uint64_t id) const = 0;
  virtual const ScriptValue* script_get(std::uint64_t id,
                                        const std::string& key) const = 0;
  // value null = erase.
  virtual void script_set(std::uint64_t id, const std::string& key,
                          const ScriptValue* value) = 0;
  // slot -1 = every rider.
  virtual void script_toast(int slot, const std::string& text, double seconds,
                            std::uint64_t from) = 0;
  virtual void script_log(LogEntry entry) = 0;
  // Plugins: add Lua globals to the sandbox table at stack index `sandbox`
  // (absolute). Default: nothing.
  // audio.play / audio.music / ... (queued for the host, see Sound.h).
  virtual void script_sound(SoundEvent /*event*/) {}
  // ui.show / ui.hide / ui.set_text / ... (queued for the host's UI).
  virtual void script_ui(UiCommand /*command*/) {}
  // particles.burst / emit / stop / start (World::fx / World3D::fx).
  virtual void script_fx(FxEvent /*event*/) {}
  virtual void script_open_extensions(lua_State* /*L*/, int /*sandbox*/) {}
  // Buttons (runtime::kButton* bits) the rider in `slot` holds this tick;
  // graphs' Key event and compiled scripts read it. Default: none.
  virtual std::uint32_t script_buttons(int /*slot*/) const { return 0; }

  // ABI 4. Defaults keep a world that only implements the 2D calls usable:
  // pose reads script_entity, move_by adds to that x/y, the rest no-op.
  // out18: px,py,pz, ex,ey,ez, sx,sy,sz, fx,fy,fz, rx,ry,rz, ux,uy,uz.
  virtual bool script_pose(std::uint64_t id, float* out18) const;
  virtual void script_move_by(std::uint64_t id, float x, float y, float z);
  virtual void script_rotate_by(std::uint64_t id, float ex, float ey, float ez);
  virtual void script_set_local_euler(std::uint64_t id, float ex, float ey, float ez);
  virtual void script_set_position(std::uint64_t id, float x, float y, float z);
  virtual float script_axis(int slot, const char* name) const;
  virtual bool script_button_down(int slot, const char* name) const;
  virtual int script_query(const char* op, const char* args, char* out, int cap);

  // Plugin call bridge: table.fn(args...) on a plugin's Lua table (the ones
  // script_open_extensions hands Lua scripts), for graphs, C++ and C#. One
  // value back (nil = false). False + *error when there is no such table /
  // function or it raised.
  bool script_plugin_call(const std::string& table, const std::string& fn,
                          const std::vector<ScriptValue>& args, ScriptValue* result,
                          std::string* error);

 private:
  struct PluginBridge;
  struct PluginBridgeDeleter {
    void operator()(PluginBridge* b) const;
  };
  std::unique_ptr<PluginBridge, PluginBridgeDeleter> plugin_bridge_;
};

// One scripting language's host (Lua, graphs, C++ modules, C#). The world
// talks to ScriptHost (ScriptHost.h), which routes each entity's script
// component to the language its file extension names.
class ScriptLanguageHost {
 public:
  virtual ~ScriptLanguageHost() = default;
  virtual const char* language() const = 0;
  // An entity can carry several scripts (its Inspector stack); `slot` is
  // the script's place in that stack (0 = first). attach() gets the entity
  // with `script` set to that slot's script. One instance per (entity,
  // slot); detach() drops every slot. World-wide phases (start, tick,
  // broadcast) are run one slot at a time by ScriptHost, so an object's
  // scripts always run in their Inspector order.
  virtual bool attach(const Entity2D& e, int slot) = 0;
  virtual void detach(std::uint64_t entity) = 0;
  virtual bool has(std::uint64_t entity, int slot) const = 0;
  virtual bool running(std::uint64_t entity, int slot) const = 0;
  virtual bool has_hook(std::uint64_t entity, int slot, const char* hook) = 0;
  virtual std::size_t instance_count() const = 0;
  virtual void start_pending(int slot) = 0;
  virtual void trigger(bool enter, std::uint64_t trigger, int slot, std::uint64_t other) = 0;
  virtual void interact(std::uint64_t target, int slot, std::uint64_t rider) = 0;
  virtual void hand_event(std::uint64_t target, int slot, const char* hook,
                          const std::string& text, double number) = 0;
  virtual void ui_event(const std::string& /*widget*/, bool /*change*/, double /*value*/,
                        const std::string& /*text*/) {}
  virtual void run_timers(std::uint64_t /*tick*/) {}
  // Call `hook(text, number)` on every running instance that defines it
  // (plugin events such as on_quest_completed); entity id order.
  virtual void broadcast(int /*slot*/, const char* /*hook*/, const std::string& /*text*/,
                         double /*number*/) {}
  virtual void tick(double dt, int slot) = 0;
  // Physics step (before tick) and the pass after tick. Default: nothing,
  // so Lua and graphs are unchanged.
  virtual void fixed_tick(double /*dt*/, int /*slot*/) {}
  virtual void late_tick(double /*dt*/, int /*slot*/) {}
  // Solid-collider overlap. phase 0 enter, 1 stay, 2 exit.
  virtual void collide(std::uint64_t /*self*/, int /*slot*/, std::uint64_t /*other*/, int /*phase*/) {}
  virtual int reload_changed() = 0;
  virtual bool reload(const std::string& /*rel*/) { return false; }
  // Some running instance uses this file.
  virtual bool uses(const std::string& /*rel*/) const { return false; }
};

// Lua 5.4 (sandboxed): the original TombStone script language.
class LuaScriptHost final : public ScriptLanguageHost {
 public:
  static constexpr std::size_t kMemoryLimit = 64u << 20;     // bytes, all scripts
  static constexpr long long kInstructionBudget = 2000000;   // per hook call
  static constexpr std::size_t kMaxTimers = 4096;            // per world
  static constexpr std::size_t kMaxStateKeys = 256;          // per entity

  LuaScriptHost(ScriptWorld& world, ScriptLibrary& library,
             std::string project_dir);
  ~LuaScriptHost() override;
  const char* language() const override { return "lua"; }
  LuaScriptHost(const LuaScriptHost&) = delete;
  LuaScriptHost& operator=(const LuaScriptHost&) = delete;

  // Give `e` its script instance and run the file's top level (props and
  // hook definitions). False (logged) when it cannot load; the instance is
  // kept, stopped, so a hot reload can bring it back.
  bool attach(const Entity2D& e, int slot) override;
  // The entity left the world: drop its instances and timers.
  void detach(std::uint64_t entity) override;
  bool has(std::uint64_t entity, int slot) const override;
  bool running(std::uint64_t entity, int slot) const override;  // loaded and not stopped
  bool has_hook(std::uint64_t entity, int slot, const char* hook) override;
  std::size_t instance_count() const override;
  std::size_t timer_count() const;
  std::size_t memory_used() const;

  // Tick phases, in World::step order.
  void start_pending(int slot) override;
  void trigger(bool enter, std::uint64_t trigger, int slot, std::uint64_t other) override;
  void interact(std::uint64_t target, int slot, std::uint64_t rider) override;
  // A hook called with (text, number): the VR hand events on_grab(hand),
  // on_release(hand, speed), on_point(hand), on_unpoint(hand),
  // on_vr_trigger(hand). Missing hooks are skipped.
  // The host's UI reports a widget event: every ui.on_click(id, fn) /
  // ui.on_change(id, fn) handler for that widget id runs fn(id, value, text).
  void ui_event(const std::string& widget, bool change, double value, const std::string& text) override;
  void broadcast(int slot, const char* hook, const std::string& text, double number) override;
  std::size_t ui_handler_count() const;
  void hand_event(std::uint64_t target, int slot, const char* hook, const std::string& text,
                  double number) override;
  void run_timers(std::uint64_t tick) override;
  void tick(double dt, int slot) override;

  // Hot reload: re-run changed files in their instances (props re-applied,
  // stopped instances restarted, on_reload called). A file that no longer
  // compiles keeps the old version and logs why. Returns files reloaded.
  int reload_changed() override;
  bool reload(const std::string& rel) override;
  bool uses(const std::string& rel) const override;

  // Props a script declares (`props = { reach = 24, ... }` at top level),
  // sorted by name. False with the error (file:line) when it does not run.
  static bool describe(const std::string& source, const std::string& rel,
                       std::vector<ScriptProp>* defaults, std::string* error);

  struct State;

 private:
  std::unique_ptr<State> s_;
};

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts

#include "runtime/ScriptHost.h"
