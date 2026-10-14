#include "editor/plugins/EditorPlugins.h"

#include "editor/projects/ProjectStore.h"
#include "plugins/Builtins.h"

#include <algorithm>
#include <iostream>

namespace ts {
namespace tombstone {
namespace editor {

using runtime::LogLevel;

plugin::ProjectType plugin_type_of(ProjectKind kind) {
  switch (kind) {
    case ProjectKind::ThreeD:
      return plugin::ProjectType::ThreeD;
    case ProjectKind::VR:
      return plugin::ProjectType::VR;
    case ProjectKind::TwoD:
      break;
  }
  return plugin::ProjectType::TwoD;
}

EditorPlugins::EditorPlugins()
    : catalog_(std::make_unique<plugin::Catalog>()),
      manager_(std::make_unique<plugin::PluginManager>(*catalog_)) {}

EditorPlugins::~EditorPlugins() { close(); }

void EditorPlugins::say(LogLevel level, const std::string& text) const {
  if (sink_) sink_(level, text);
}

void EditorPlugins::build_catalog() {
  manager_.reset();
  catalog_ = std::make_unique<plugin::Catalog>();
  catalog_errors_.clear();
  plugins::fill_catalog(*catalog_, project_ ? project_->path : std::string(),
                        &catalog_errors_);
  register_editor_builtin_plugins(*catalog_, &catalog_errors_);
  manager_ = std::make_unique<plugin::PluginManager>(*catalog_);
  manager_->set_log_sink([this](const plugin::Diagnostic& d) {
    say(d.level, d.plugin.empty() ? d.text : "[" + d.plugin + "] " + d.text);
  });
  for (const std::string& e : catalog_errors_) {
    say(LogLevel::Error, "Plugin wagon: " + e);
  }
}

void EditorPlugins::open(ProjectInfo* project, std::string host,
                         void* host_object, Sink sink) {
  close();
  project_ = project;
  host_ = std::move(host);
  host_object_ = host_object;
  sink_ = std::move(sink);
  build_catalog();
  const plugin::ProjectType type = plugin_type_of(project_->kind);
  enabled_ = project_->plugins_set
                 ? project_->plugins
                 : plugin::PluginManager::defaults(*catalog_, type);
  apply();
}

void EditorPlugins::close() {
  if (manager_) manager_->unload_all();
  project_ = nullptr;
  host_object_ = nullptr;
  enabled_.clear();
  loaded_list_.clear();
  sink_ = nullptr;
}

bool EditorPlugins::is_enabled(const std::string& id) const {
  return std::find(enabled_.begin(), enabled_.end(), id) != enabled_.end();
}

bool EditorPlugins::save_project(std::string* error) {
  if (!project_) return false;
  project_->plugins = enabled_;
  project_->plugins_set = true;
  if (project_->path.empty()) return true;  // in-memory project (tests)
  std::string err;
  if (!ProjectStore::write_project_json(*project_, &err)) {
    if (error) *error = err;
    say(LogLevel::Error, "Couldn't save the plugin list: " + err);
    return false;
  }
  return true;
}

bool EditorPlugins::set_enabled(const std::string& id, bool on,
                                std::string* error) {
  if (!project_) {
    if (error) *error = "no project open";
    return false;
  }
  const plugin::CatalogEntry* entry = catalog_->find(id);
  if (!entry && on) {
    if (error) *error = "no plugin '" + id + "' in the wagon";
    return false;
  }
  if (on == is_enabled(id)) return true;

  if (on) {
    // Required dependencies first (recursively).
    std::vector<std::string> stack{id};
    std::vector<std::string> added;
    while (!stack.empty()) {
      const std::string cur = stack.back();
      stack.pop_back();
      if (is_enabled(cur)) continue;
      enabled_.push_back(cur);
      if (cur != id) added.push_back(cur);
      if (const plugin::CatalogEntry* e = catalog_->find(cur)) {
        for (const plugin::Dependency& d : e->info.deps) {
          if (!d.optional && catalog_->find(d.id) && !is_enabled(d.id)) {
            stack.push_back(d.id);
          }
        }
      }
    }
    for (const std::string& a : added) {
      const plugin::CatalogEntry* e = catalog_->find(a);
      say(LogLevel::Info, entry->info.name + " rides with " +
                              (e ? e->info.name : a) + ": hitched that up too");
    }
  } else {
    // Everything that (transitively) requires `id` comes off as well.
    std::vector<std::string> off{id};
    bool grew = true;
    while (grew) {
      grew = false;
      for (const std::string& en : enabled_) {
        if (std::find(off.begin(), off.end(), en) != off.end()) continue;
        const plugin::CatalogEntry* e = catalog_->find(en);
        if (!e) continue;
        for (const plugin::Dependency& d : e->info.deps) {
          if (!d.optional && std::find(off.begin(), off.end(), d.id) != off.end()) {
            off.push_back(en);
            grew = true;
            break;
          }
        }
      }
    }
    for (std::size_t i = 1; i < off.size(); ++i) {
      const plugin::CatalogEntry* e = catalog_->find(off[i]);
      say(LogLevel::Info, (e ? e->info.name : off[i]) + " can't ride without " +
                              (entry ? entry->info.name : id) +
                              ": unhitched it too");
    }
    enabled_.erase(std::remove_if(enabled_.begin(), enabled_.end(),
                                  [&](const std::string& s) {
                                    return std::find(off.begin(), off.end(), s) != off.end();
                                  }),
                   enabled_.end());
  }
  // Keep the saved list in catalog order (stable diffs in project.json);
  // unknown ids (a missing DLL) stay at the end so they are not lost.
  std::vector<std::string> ordered;
  for (const plugin::CatalogEntry& e : catalog_->entries()) {
    if (is_enabled(e.info.id)) ordered.push_back(e.info.id);
  }
  for (const std::string& s : enabled_) {
    if (!catalog_->find(s)) ordered.push_back(s);
  }
  enabled_ = std::move(ordered);
  return save_project(error);
}

std::size_t EditorPlugins::apply() {
  if (!project_) return 0;
  plugin::PluginContext ctx;
  ctx.project_type = plugin_type_of(project_->kind);
  ctx.project_dir = project_->path;
  ctx.host = host_;
  ctx.host_object = host_object_;
  const std::size_t n = manager_->load(enabled_, ctx);
  loaded_list_ = enabled_;
  std::cout << "[Plugins] " << n << " hitched for " << project_->name << '\n';
  return n;
}

std::size_t EditorPlugins::rescan() {
  if (!project_) return 0;
  manager_->unload_all();
  build_catalog();
  return apply();
}

std::string EditorPlugins::skip_reason(const std::string& id) const {
  std::string last;
  for (const plugin::Diagnostic& d : manager_->diagnostics()) {
    if (d.plugin == id && d.level != LogLevel::Info) last = d.text;
  }
  return last;
}

std::vector<std::string> EditorPlugins::default_ids(ProjectKind kind) {
  plugin::Catalog c;
  plugins::register_builtin_plugins(c, nullptr);
  register_editor_builtin_plugins(c, nullptr);
  return plugin::PluginManager::defaults(c, plugin_type_of(kind));
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
