#pragma once

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace launcher {

// Full path of the running executable ("" when the platform will not say).
std::string self_executable_path();

// Where ts_game should be, relative to the running ts_admin: the same
// folder, or the CMake build tree next door (build/apps/game, plus the
// config folder on multi-config generators). Every candidate in order.
std::vector<std::string> game_executable_candidates();
// First candidate that exists, or "" when ts_game is not built.
std::string find_game_executable();

// Start `exe --project <project_dir>` as its own process and return
// without waiting (it outlives the editor). False + reason on failure.
bool spawn_game(const std::string& exe, const std::string& project_dir,
                std::string* error_out = nullptr);

// ts_client next to ts_admin, like ts_game above (build/apps/client).
std::vector<std::string> client_executable_candidates();
std::string find_client_executable();

// Start `exe --connect <endpoint> --name <name> [--project <dir>]` as its
// own process (a windowed posse rider). False + reason on failure.
bool spawn_client(const std::string& exe, const std::string& endpoint,
                  const std::string& name, const std::string& project_dir,
                  std::string* error_out = nullptr);

}  // namespace launcher
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
