#pragma once

// Loads a project's enabled plugins from a Catalog in dependency order and
// collects what they register. Problems never throw: each becomes a
// Diagnostic (sent to the log sink -- the editor's Telegraph -- and kept
// in diagnostics()), and the offending plugin is skipped while the rest
// load.
//
// Rules (resolve()):
//  * an enabled id the catalog lacks: warning, skipped
//  * a plugin that does not fit the project type: warning, skipped
//  * a required dependency that is missing, disabled, the wrong version
//    (same major, >= min) or itself skipped: error, skipped (cascades)
//  * optional dependencies only order the load when present
//  * a dependency loop: error, every plugin in it skipped
//  * order: dependencies first, otherwise catalog order (deterministic)

#include "plugin/Catalog.h"
#include "plugin/Plugin.h"
#include "runtime/Log.h"
#include "runtime/WorldExtensions.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace plugin {

struct Diagnostic {
  runtime::LogLevel level = runtime::LogLevel::Info;
  std::string plugin;  // id it concerns ("" = the manager)
  std::string text;
};

struct Resolution {
  std::vector<std::string> order;    // load order
  std::vector<std::string> skipped;  // enabled but not loading
  std::vector<Diagnostic> diagnostics;
  bool has_errors() const;
};

template <typename T>
struct Owned {
  std::string owner;  // plugin id
  T item;
};

class PluginManager {
 public:
  using LogSink = std::function<void(const Diagnostic&)>;

  explicit PluginManager(const Catalog& catalog);
  ~PluginManager();
  PluginManager(const PluginManager&) = delete;
  PluginManager& operator=(const PluginManager&) = delete;

  void set_log_sink(LogSink sink) { sink_ = std::move(sink); }
  const Catalog& catalog() const { return catalog_; }

  Resolution resolve(const std::vector<std::string>& enabled,
                     ProjectType type) const;

  // Unload everything, then resolve + load. Returns the number loaded.
  std::size_t load(const std::vector<std::string>& enabled,
                   const PluginContext& ctx);
  // on_unload in reverse order, then destroy. Contributions are dropped:
  // stop any ride using world_extensions() first.
  void unload_all();

  const std::vector<std::string>& load_order() const { return order_; }
  bool loaded(const std::string& id) const;
  Plugin* get(const std::string& id) const;
  const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }
  std::size_t error_count() const;
  const PluginContext& context() const { return ctx_; }

  // Contributions of the loaded plugins, in load order.
  std::shared_ptr<const runtime::WorldExtensions> world_extensions() const {
    return world_;
  }
  const std::vector<Owned<EditorPanel>>& editor_panels() const { return panels_; }
  const std::vector<Owned<EditorMenuItem>>& menu_items() const { return menus_; }
  const std::vector<Owned<AssetType>>& asset_types() const { return assets_; }
  // Asset type label for a file name by its (longest) registered suffix,
  // or nullptr.
  const Owned<AssetType>* asset_type_for(const std::string& filename) const;

  // Ride lifecycle, forwarded to every loaded plugin in load order (end:
  // reverse). Exceptions are caught and logged.
  void play_begin(runtime::World& world);
  void play_end();
  bool in_play() const { return in_play_; }

  // Plugins a new (or pre-plugin) project of `type` starts with:
  // default_enabled ones that fit, catalog order.
  static std::vector<std::string> defaults(const Catalog& catalog,
                                           ProjectType type);

 private:
  class Collector;
  struct Loaded {
    std::string id;
    Plugin* plugin = nullptr;
    const CatalogEntry* entry = nullptr;
  };

  void report(runtime::LogLevel level, const std::string& plugin,
              std::string text);

  const Catalog& catalog_;
  LogSink sink_;
  PluginContext ctx_;
  std::vector<Loaded> loaded_;
  std::vector<std::string> order_;
  std::vector<Diagnostic> diagnostics_;
  std::shared_ptr<runtime::WorldExtensions> world_;
  std::vector<Owned<EditorPanel>> panels_;
  std::vector<Owned<EditorMenuItem>> menus_;
  std::vector<Owned<AssetType>> assets_;
  bool in_play_ = false;
};

// "a, b ,c" -> {"a","b","c"} (empty items dropped, duplicates kept once).
std::vector<std::string> split_plugin_list(const std::string& text);
std::string join_plugin_list(const std::vector<std::string>& ids);

}  // namespace plugin
}  // namespace tombstone
}  // namespace ts
