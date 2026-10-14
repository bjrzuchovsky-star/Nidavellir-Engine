#pragma once

// Gameplay scripts: sandboxed Lua 5.4, run by the world on its fixed tick.
//
// Each entity with a `script` component gets its own instance of the file
// (own globals, own `self`, own `props`). Hooks the file may define:
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

// Scripts tick with the world (World::kTickRate; checked where they meet).
inline constexpr int kScriptTickRate = 60;

// "scripts/gate.lua"-style: relative, forward slashes, no "." / ".."
// parts, ends in ".lua".
bool valid_script_path(const std::string& rel);

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
  virtual void script_open_extensions(lua_State* /*L*/, int /*sandbox*/) {}
};

class ScriptHost {
 public:
  static constexpr std::size_t kMemoryLimit = 64u << 20;     // bytes, all scripts
  static constexpr long long kInstructionBudget = 2000000;   // per hook call
  static constexpr std::size_t kMaxTimers = 4096;            // per world
  static constexpr std::size_t kMaxStateKeys = 256;          // per entity

  ScriptHost(ScriptWorld& world, ScriptLibrary& library,
             std::string project_dir);
  ~ScriptHost();
  ScriptHost(const ScriptHost&) = delete;
  ScriptHost& operator=(const ScriptHost&) = delete;

  // Give `e` its script instance and run the file's top level (props and
  // hook definitions). False (logged) when it cannot load; the instance is
  // kept, stopped, so a hot reload can bring it back.
  bool attach(const Entity2D& e);
  // The entity left the world: drop its instance and timers.
  void detach(std::uint64_t entity);
  bool has(std::uint64_t entity) const;
  bool running(std::uint64_t entity) const;  // loaded and not stopped
  bool has_hook(std::uint64_t entity, const char* hook);
  std::size_t instance_count() const;
  std::size_t timer_count() const;
  std::size_t memory_used() const;

  // Tick phases, in World::step order.
  void start_pending();
  void trigger(bool enter, std::uint64_t trigger, std::uint64_t other);
  void interact(std::uint64_t target, std::uint64_t rider);
  // A hook called with (text, number): the VR hand events on_grab(hand),
  // on_release(hand, speed), on_point(hand), on_unpoint(hand),
  // on_vr_trigger(hand). Missing hooks are skipped.
  void hand_event(std::uint64_t target, const char* hook, const std::string& text,
                  double number);
  void run_timers(std::uint64_t tick);
  void tick(double dt);

  // Hot reload: re-run changed files in their instances (props re-applied,
  // stopped instances restarted, on_reload called). A file that no longer
  // compiles keeps the old version and logs why. Returns files reloaded.
  int reload_changed();
  bool reload(const std::string& rel);

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
