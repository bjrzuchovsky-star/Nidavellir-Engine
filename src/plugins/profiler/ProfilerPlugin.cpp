#include "plugins/profiler/ProfilerPlugin.h"

#include "plugin/Catalog.h"
#include "profiler/Profiler.h"
#include "profiler/ProfilerLua.h"
#include "runtime/World.h"

#include <algorithm>
#include <filesystem>

namespace ts {
namespace tombstone {
namespace plugins {

namespace {
ProfilerPlugin::EditorHook& hook_ref() {
  static ProfilerPlugin::EditorHook h;
  return h;
}
}  // namespace

const plugin::PluginInfo& ProfilerPlugin::static_info() {
  static const plugin::PluginInfo info = [] {
    plugin::PluginInfo i;
    i.id = "ts.profiler";
    i.name = "Assay Office (Profiler)";
    i.description =
        "Frame profiler and debug tools: named CPU scopes per system, script language and plugin, a "
        "flame / timeline view, GPU pass timings, draw calls, memory and asset counts, net stats, an "
        "in-game overlay (F3) and console (backtick), captures to captures/*.tsprof.json, and the "
        "`profiler` / `console` Lua tables.";
    i.author = "TombStone";
    i.version = plugin::Version{1, 0, 0};
    i.category = plugin::PluginCategory::Engine;
    i.project_types = plugin::kType2D | plugin::kType3D | plugin::kTypeVR;
    i.default_enabled = false;
    return i;
  }();
  return info;
}

void ProfilerPlugin::set_editor_hook(EditorHook hook) { hook_ref() = std::move(hook); }

bool ProfilerPlugin::on_load(const plugin::PluginContext& ctx, std::string* /*error*/) {
  host_ = ctx.host;
  if (!ctx.project_dir.empty()) profiler::Profiler::get().set_capture_dir(capture_dir_for(ctx.project_dir));
  return true;
}

void ProfilerPlugin::on_register(plugin::Registrar& reg) {
  reg.add_lua_api("profiler", [](lua_State* L) { profiler::push_profiler_lua(L); });
  reg.add_lua_api("console", [](lua_State* L) { profiler::push_console_lua(L); });
  if (hook_ref() && host_.rfind("editor", 0) == 0) hook_ref()(reg);
}

void ProfilerPlugin::on_play_begin(runtime::World& /*world*/) {
  profiler::Profiler& p = profiler::Profiler::get();
  if (!p.enabled()) {
    p.set_enabled(true);
    switched_on_ = true;
  }
}

void ProfilerPlugin::on_play_end() {
  // Keep the history for the panels; stop recording if we started it.
  if (switched_on_) profiler::Profiler::get().set_enabled(false);
  switched_on_ = false;
}

void ProfilerPlugin::on_unload() { on_play_end(); }

bool profiler_enabled(const std::vector<std::string>& ids) {
  return std::find(ids.begin(), ids.end(), "ts.profiler") != ids.end();
}

std::string capture_dir_for(const std::string& project_dir) {
  return (std::filesystem::path(project_dir) / "captures").string();
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts

TS_PLUGIN_STATIC(profiler, ::ts::tombstone::plugins::ProfilerPlugin)
