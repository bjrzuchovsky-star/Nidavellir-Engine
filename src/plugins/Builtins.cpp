#include "plugins/Builtins.h"

#include <cstdlib>
#include <filesystem>

// Static plugin links (TS_PLUGIN_STATIC in each plugin's .cpp).
TS_PLUGIN_DECLARE(tumbleweed);

namespace ts {
namespace tombstone {
namespace plugins {

void register_builtin_plugins(plugin::Catalog& catalog,
                              std::vector<std::string>* errors) {
  std::string err;
  if (!TS_PLUGIN_LINK(tumbleweed, catalog, &err) && errors) {
    errors->push_back(err);
  }
}

std::vector<std::string> plugin_search_dirs(const std::string& project_dir) {
  std::vector<std::string> dirs;
  if (const char* env = std::getenv("TS_PLUGIN_PATH")) {
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string cur;
    for (const char* c = env;; ++c) {
      if (*c == sep || *c == ';' || *c == '\0') {
        if (!cur.empty()) dirs.push_back(cur);
        cur.clear();
        if (*c == '\0') break;
      } else {
        cur += *c;
      }
    }
  }
  if (!project_dir.empty()) {
    dirs.push_back((std::filesystem::path(project_dir) / "plugins").string());
  }
#if defined(TS_SAMPLE_PLUGIN_DIR)
  dirs.push_back(TS_SAMPLE_PLUGIN_DIR);
#endif
  return dirs;
}

void fill_catalog(plugin::Catalog& catalog, const std::string& project_dir,
                  std::vector<std::string>* errors) {
  register_builtin_plugins(catalog, errors);
  for (const std::string& dir : plugin_search_dirs(project_dir)) {
    catalog.scan_folder(dir, errors);
  }
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
