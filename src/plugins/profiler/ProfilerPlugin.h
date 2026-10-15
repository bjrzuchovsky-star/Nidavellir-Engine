#pragma once

// ts.profiler "Assay Office (Profiler)": optional (off by default). When a
// project enables it:
//   - the frame profiler runs during Play (CPU scopes per system, script
//     language and plugin, counters, GPU timings), captures go to
//     <project>/captures/;
//   - the `profiler` and `console` Lua tables are available;
//   - hosts add the in-game debug overlay (F3) and console (backtick);
//   - the editor gets the Assay Office panels (frame timeline, render &
//     memory, net) through the editor hook.

#include "plugin/Plugin.h"

#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace plugins {

class ProfilerPlugin : public plugin::Plugin {
 public:
  static const plugin::PluginInfo& static_info();
  const plugin::PluginInfo& info() const override { return static_info(); }
  bool on_load(const plugin::PluginContext& ctx, std::string* error) override;
  void on_register(plugin::Registrar& reg) override;
  void on_play_begin(runtime::World& world) override;
  void on_play_end() override;
  void on_unload() override;

  using EditorHook = std::function<void(plugin::Registrar&)>;
  static void set_editor_hook(EditorHook hook);

 private:
  std::string host_;
  bool switched_on_ = false;
};

// The project enables ts.profiler (ids from read_project_plugins).
bool profiler_enabled(const std::vector<std::string>& enabled_plugin_ids);
// <project>/captures
std::string capture_dir_for(const std::string& project_dir);

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
