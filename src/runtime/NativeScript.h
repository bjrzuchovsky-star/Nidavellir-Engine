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

#include "runtime/CompiledScript.h"

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

using BuildResult = ScriptBuildResult;
// Writes <project>/.tombstone/native/{CMakeLists.txt, module_main.cpp,
// sdk/...} and builds with cmake (Release). force = even when up to date.
BuildResult build(const std::string& project_dir, bool force = false);
// cmake found (TS_CMAKE_COMMAND, else on PATH)?
bool toolchain_available(std::string* why);
// The script SDK include folder (has tombstone/script_sdk.h), "" when missing.
std::string sdk_include_dir();
// Default false: the editor / ts_game build modules on a worker thread and
// swap them in when ready (the tick never waits). Smokes, servers and
// exports can ask for the old synchronous builds.
void set_blocking_builds(bool on);
bool blocking_builds();

}  // namespace native_scripts

class NativeScriptHost final : public CompiledScriptHost {
 public:
  NativeScriptHost(ScriptWorld& world, ScriptLibrary& library, std::string project_dir);
};

}  // namespace runtime
}  // namespace tombstone
}  // namespace ts
