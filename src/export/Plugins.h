#pragma once

// Which plugins a project enables and where their dynamic libraries live.
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace exporter {

struct PluginNeed {
  std::string id;
  bool found = false;
  bool dynamic = false;
  bool editor_only = false;
  std::string library;  // dynamic: the DLL / .so to ship
};

// Enabled list (project.json "plugins", else the defaults for its kind),
// resolved against built-ins + plugin_search_dirs(project) + extra_dirs.
std::vector<PluginNeed> project_plugins(const std::string& project_dir,
                                        const std::vector<std::string>& extra_dirs);

}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
