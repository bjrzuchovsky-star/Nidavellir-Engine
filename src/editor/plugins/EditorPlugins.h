#pragma once

// The open project's plugins, as the editor sees them: a Catalog (runtime
// built-ins + editor built-ins + dynamic libraries from the plugin search
// dirs), a PluginManager loaded with the project's enabled list, and the
// enable / disable bookkeeping that writes project.json.
//
// Restart-of-play semantics: toggling a plugin saves the list at once, but
// the loaded set only changes on apply(), which the editor runs when no
// ride (Play or posse) is going -- immediately when idle, otherwise as the
// ride stops. pending() says a change is waiting.

#include "editor/ProjectInfo.h"
#include "plugin/Catalog.h"
#include "plugin/PluginManager.h"
#include "runtime/Log.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

// Editor-side built-ins (the posse). Defined with the plugins themselves.
void register_editor_builtin_plugins(plugin::Catalog& catalog,
                                     std::vector<std::string>* errors);

plugin::ProjectType plugin_type_of(ProjectKind kind);

class EditorPlugins {
 public:
  using Sink = std::function<void(runtime::LogLevel, const std::string&)>;

  EditorPlugins();
  ~EditorPlugins();
  EditorPlugins(const EditorPlugins&) = delete;
  EditorPlugins& operator=(const EditorPlugins&) = delete;

  // Fill the catalog for `project` and load its enabled plugins. `project`
  // must outlive this object (the editor screen owns both). host names the
  // screen ("editor2d", "frontier"); host_object is handed to plugins.
  void open(ProjectInfo* project, std::string host, void* host_object,
            Sink sink);
  void close();
  bool is_open() const { return project_ != nullptr; }

  plugin::Catalog& catalog() { return *catalog_; }
  const plugin::Catalog& catalog() const { return *catalog_; }
  plugin::PluginManager& manager() { return *manager_; }
  const plugin::PluginManager& manager() const { return *manager_; }
  const std::vector<std::string>& catalog_errors() const { return catalog_errors_; }

  // Effective enabled list (the project's, or the defaults for old ones).
  const std::vector<std::string>& enabled() const { return enabled_; }
  bool is_enabled(const std::string& id) const;
  bool loaded(const std::string& id) const { return manager_->loaded(id); }

  // Switch a plugin on / off and save project.json. Switching on also
  // switches on its required dependencies; switching off also switches
  // off what requires it. Each extra change is reported to the sink.
  // False (with *error) when the id is unknown or saving failed.
  bool set_enabled(const std::string& id, bool on, std::string* error = nullptr);
  bool pending() const { return enabled_ != loaded_list_; }
  // Reload the manager with the enabled list. The caller makes sure no
  // ride holds world_extensions(). Returns plugins loaded.
  std::size_t apply();
  // Rebuild the catalog (picks up new / removed plugin libraries), then
  // apply(). Same no-ride rule.
  std::size_t rescan();

  // 0.2.0 split: the physics / movement plugins the open scene needs
  // (editor/plugins/PluginNeeds.h). set_needs_baseline records what the
  // scene used when it opened; follow_needs switches on plugins the scene
  // started to need since the last call (a Player, Rigid Body, Grabbable,
  // Joint or VR Rig was added) and logs it. Plugins switched off while the
  // scene already used them stay off. Returns the ids switched on.
  void set_needs_baseline(const std::vector<std::string>& needs) { seen_needs_ = needs; }
  std::vector<std::string> follow_needs(const std::vector<std::string>& needs);

  // Why an enabled plugin is not loaded (from the last load), or "".
  std::string skip_reason(const std::string& id) const;

  // Plugins a brand-new project of `kind` starts with (built-ins only).
  static std::vector<std::string> default_ids(ProjectKind kind);

 private:
  void build_catalog();
  void say(runtime::LogLevel level, const std::string& text) const;
  bool save_project(std::string* error);

  // Catalog first: members are destroyed in reverse, so the manager (whose
  // plugins may live in the catalog's libraries) goes first.
  std::unique_ptr<plugin::Catalog> catalog_;
  std::unique_ptr<plugin::PluginManager> manager_;
  std::vector<std::string> catalog_errors_;
  ProjectInfo* project_ = nullptr;
  std::string host_;
  void* host_object_ = nullptr;
  Sink sink_;
  std::vector<std::string> enabled_;
  std::vector<std::string> loaded_list_;  // enabled_ as of the last apply()
  std::vector<std::string> seen_needs_;   // follow_needs bookkeeping
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
