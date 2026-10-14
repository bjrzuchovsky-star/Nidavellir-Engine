#pragma once

// Plugins for hosts without the editor (ts_game; later ts_server): read the
// project's "dimension" and "plugins" from project.json (absent = the
// defaults for its type), fill a catalog with the runtime built-ins plus
// dynamic plugins from the search dirs, and load. Editor-only plugins (the
// posse) are skipped quietly with an info line.
//
//   RuntimePlugins rp;
//   rp.open(project_dir, "game", sink);
//   rp.attach(session.world());        // before start / build
//   session.start_project(project_dir);
//   rp.begin(session.world());         // each ride; rp.end() after

#include "plugin/Catalog.h"
#include "plugin/PluginManager.h"

#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace plugins {

struct ProjectPluginList {
  plugin::ProjectType type = plugin::ProjectType::TwoD;
  std::vector<std::string> ids;
  bool listed = false;  // "plugins" present in project.json
};

// Missing / unreadable project.json -> 2D, not listed.
ProjectPluginList read_project_plugins(const std::string& project_dir);

class RuntimePlugins {
 public:
  using Sink = plugin::PluginManager::LogSink;

  RuntimePlugins();
  ~RuntimePlugins();
  RuntimePlugins(const RuntimePlugins&) = delete;
  RuntimePlugins& operator=(const RuntimePlugins&) = delete;

  // Returns plugins loaded.
  std::size_t open(const std::string& project_dir, const std::string& host,
                   Sink sink);
  void attach(runtime::World& world) const;
  void begin(runtime::World& world) { manager_->play_begin(world); }
  void end() { manager_->play_end(); }

  plugin::Catalog& catalog() { return *catalog_; }
  plugin::PluginManager& manager() { return *manager_; }

 private:
  std::unique_ptr<plugin::Catalog> catalog_;
  std::unique_ptr<plugin::PluginManager> manager_;
};

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
