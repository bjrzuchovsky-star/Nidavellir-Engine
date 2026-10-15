#pragma once

// Compiled script languages (C++ modules, C#) share one host: every
// instance is an object behind the C ABI (script_abi.h), the project's
// scripts build into one module, builds run off the game tick (worker
// thread, swapped in when ready), hot reload = save() -> new module ->
// load() -> on_reload(), build errors go to the Telegraph. A backend says
// how one language builds and opens its module.

#include "runtime/Script.h"
#include "tombstone/script_abi.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

struct ScriptBuildResult {
  bool ok = false;
  bool skipped = false;          // no sources, or nothing changed
  std::string module;            // built file
  std::string output;            // raw build output
  std::vector<LogEntry> problems;
  std::string error;             // why it failed (summary)
};

// Default false: modules build on a worker thread and swap in when ready
// (the tick never waits). Smokes and tools can ask for synchronous builds.
void set_compiled_scripts_blocking(bool on);
bool compiled_scripts_blocking();

// One loaded module (a shared library, a .NET assembly generation).
struct CompiledModule {
  virtual ~CompiledModule();
  std::string path;
  virtual void* create(const char* cls, const TsScriptApi* api, std::uint64_t self, const char* props) = 0;
  virtual void destroy(void* obj) = 0;
  virtual void call(void* obj, int hook, std::uint64_t entity, double number, const char* text) = 0;
  virtual int save(void* obj, char* buf, int cap) = 0;
  virtual void load(void* obj, const char* data, int len) = 0;
};

struct CompiledBackend {
  std::string lang;   // "cpp"
  std::string label;  // "C++"
  std::function<std::int64_t(const std::string& project)> stamp;
  std::function<ScriptBuildResult(const std::string& project, bool force)> build;  // any thread
  std::function<std::string(const std::string& project)> prebuilt;               // shipped module
  std::function<std::unique_ptr<CompiledModule>(const std::string& file, bool prebuilt, int generation,
                                                std::string* error)>
      open;
  // Null = always usable. False + *why (a Telegraph warning) when the
  // language cannot run here (C# without .NET).
  std::function<bool(bool prebuilt, std::string* why)> usable;
  std::function<std::string(const std::string& stem)> class_hint;
};

class CompiledScriptHost : public ScriptLanguageHost {
 public:
  CompiledScriptHost(ScriptWorld& world, std::string project_dir, CompiledBackend backend);
  ~CompiledScriptHost() override;

  const char* language() const override;
  bool attach(const Entity2D& e) override;
  void detach(std::uint64_t entity) override;
  bool has(std::uint64_t entity) const override;
  bool running(std::uint64_t entity) const override;
  bool has_hook(std::uint64_t entity, const char* hook) override;
  std::size_t instance_count() const override;
  void start_pending() override;
  void trigger(bool enter, std::uint64_t trigger, std::uint64_t other) override;
  void interact(std::uint64_t target, std::uint64_t rider) override;
  void hand_event(std::uint64_t target, const char* hook, const std::string& text,
                  double number) override;
  void ui_event(const std::string& widget, bool change, double value,
                const std::string& text) override;
  void broadcast(const char* hook, const std::string& text, double number) override;
  void tick(double dt) override;
  // Rebuilds + reloads when the sources changed (in the background unless
  // blocking). 1 when it swapped a new module in.
  int reload_changed() override;
  bool reload(const std::string& rel) override;
  bool uses(const std::string& rel) const override;

  bool module_loaded() const;
  const std::string& module_path() const;
  bool building() const;      // a background build is running
  bool wait_build();          // block until it lands; module loaded?

  struct Impl;

 private:
  std::unique_ptr<Impl> m_;
};

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
