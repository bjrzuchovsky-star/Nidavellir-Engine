#pragma once

// The plugins that ship inside the engine binaries (no DLL needed). Hosts
// call register_builtin_plugins() once on a fresh Catalog; the editor adds
// its own editor-side built-ins (the posse) on top.

#include "plugin/Catalog.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace plugins {

// Adds every runtime built-in. Problems (duplicate ids...) go to *errors.
void register_builtin_plugins(plugin::Catalog& catalog,
                              std::vector<std::string>* errors = nullptr);

// Folders a host scans for dynamic plugins, in order: $TS_PLUGIN_PATH
// (';' or ':' separated, ';' only on Windows), <project_dir>/plugins, and
// the build-tree sample folder when compiled in. Missing ones are fine.
std::vector<std::string> plugin_search_dirs(const std::string& project_dir);

// register_builtin_plugins + scan_folder over plugin_search_dirs().
void fill_catalog(plugin::Catalog& catalog, const std::string& project_dir,
                  std::vector<std::string>* errors = nullptr);

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
