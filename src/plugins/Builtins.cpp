#include "plugins/Builtins.h"

#include "plugins/survival/SurvivalPlugin.h"
#include "runtime/World.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

// Static plugin links (TS_PLUGIN_STATIC in each plugin's .cpp).
TS_PLUGIN_DECLARE(tumbleweed);
TS_PLUGIN_DECLARE(frontier);
TS_PLUGIN_DECLARE(shipyard);

namespace ts {
namespace tombstone {
namespace plugins {

void register_builtin_plugins(plugin::Catalog& catalog,
                              std::vector<std::string>* errors) {
  std::string err;
  if (!TS_PLUGIN_LINK(tumbleweed, catalog, &err) && errors) {
    errors->push_back(err);
  }
  err.clear();
  if (!TS_PLUGIN_LINK(frontier, catalog, &err) && errors) {
    errors->push_back(err);
  }
  err.clear();
  if (!TS_PLUGIN_LINK(shipyard, catalog, &err) && errors) {
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
  const std::string cache = plugin_cache_root();
  std::error_code ec;
  if (!cache.empty() && std::filesystem::is_directory(cache, ec)) {
    std::vector<std::string> installed;
    for (const auto& e : std::filesystem::directory_iterator(cache, ec)) {
      if (!e.is_directory(ec)) continue;
      if (std::filesystem::exists(e.path() / ".remove", ec)) continue;  // uninstalled
      if (!std::filesystem::exists(e.path() / "installed.json", ec)) continue;
      installed.push_back(e.path().string());
    }
    std::sort(installed.begin(), installed.end());
    dirs.insert(dirs.end(), installed.begin(), installed.end());
  }
#if defined(TS_SAMPLE_PLUGIN_DIR)
  dirs.push_back(TS_SAMPLE_PLUGIN_DIR);
#endif
  return dirs;
}

std::string plugin_cache_root() {
  const char* env = std::getenv("TS_PLUGIN_CACHE");
  return env ? std::string(env) : std::string();
}

void fill_catalog(plugin::Catalog& catalog, const std::string& project_dir,
                  std::vector<std::string>* errors) {
  register_builtin_plugins(catalog, errors);
  for (const std::string& dir : plugin_search_dirs(project_dir)) {
    catalog.scan_folder(dir, errors);
  }
}

bool select_hotbar(runtime::World& world, int slot, int index) {
  survival::SurvivalPlugin* p = survival::SurvivalPlugin::current();
  if (!p || !p->sim().active() || p->world() != &world) return false;
  p->sim().select(world, slot, index);
  return true;
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
