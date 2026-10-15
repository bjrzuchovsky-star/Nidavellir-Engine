#pragma once

// C++ gameplay scripts: every <project>/scripts/*.cpp is compiled into one
// per-project module (ts_scripts.dll / .so) against the header-only SDK
// (sdk/include/tombstone/script_sdk.h) and loaded through the C ABI
// (script_abi.h). A script component naming scripts/door.cpp gets an
// instance of the class registered with TS_SCRIPT(Door, "door").
//
// Module lookup: <project>/native/ts_scripts<ext> (prebuilt, what exports
// ship) wins; else the dev build <project>/.tombstone/native/bin/, built on
// demand (CMake project generated per claim, cmake + the host compiler)
// when missing or older than the sources. Hot reload rebuilds on save:
// save() every instance -> load the new module (a fresh copy, so the old
// file is never locked) -> load() -> on_reload(). Build errors go to the
// Telegraph with file:line; a failed build keeps the old module riding.

#include "runtime/Script.h"

#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace runtime {

namespace native_scripts {

std::string module_file_name();  // "ts_scripts.dll" / "ts_scripts.so"
std::string prebuilt_module(const std::string& project_dir);  // <p>/native/...
std::string dev_module(const std::string& project_dir);       // <p>/.tombstone/native/bin/...
bool has_sources(const std::string& project_dir);
// Hash of scripts/*.cpp|h|hpp names, times and sizes (changes on save /
// add / remove).
std::int64_t sources_stamp(const std::string& project_dir);

struct BuildResult {
  bool ok = false;
  bool skipped = false;          // no sources, or nothing changed
  std::string module;            // built file
  std::string output;            // raw cmake / compiler output
  std::vector<LogEntry> problems;
  std::string error;             // why it failed (summary)
};
// Writes <project>/.tombstone/native/{CMakeLists.txt, module_main.cpp,
// sdk/...} and builds with cmake (Release). force = even when up to date.
BuildResult build(const std::string& project_dir, bool force = false);
// cmake found (TS_CMAKE_COMMAND, else on PATH)?
bool toolchain_available(std::string* why);

}  // namespace native_scripts

class NativeScriptHost final : public ScriptLanguageHost {
 public:
  NativeScriptHost(ScriptWorld& world, ScriptLibrary& library, std::string project_dir);
  ~NativeScriptHost() override;

  const char* language() const override { return "cpp"; }
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
  void tick(double dt) override;
  // Rebuilds + reloads when scripts/*.cpp changed. 1 when it reloaded.
  int reload_changed() override;
  bool reload(const std::string& rel) override;
  bool uses(const std::string& rel) const override;

  bool module_loaded() const;
  const std::string& module_path() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> m_;
};

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
