#pragma once

// The story plugins, each optional (none is on by default):
//   ts.story     "Story Core": shared variables (`story` Lua table), the
//                condition / action language, and the per-world runtime
//                the other three plug into. A dependency of all three.
//   ts.quests    "Bounty Board (Quests)": quests/*.quest.json, objectives,
//                chains, rewards, journal + tracker UI, `quests` table.
//   ts.dialogue  "Parley (Dialogue)": dialogue/*.dlg.json branching talk,
//                conditions / actions, dialogue box UI, `dialogue` table.
//   ts.saves     "Strongbox (Saves)": save slots + rotating autosave of
//                story state and the rider's position, slot menu UI,
//                `saves` table.
// In 2D the runtime rides each Local / Server world (not clients); in 3D
// hosts attach story3d::StoryWorld3D with the same features.

#include "plugin/Plugin.h"
#include "story/StoryRuntime.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace ts {
namespace tombstone {
namespace plugins {

class StoryPlugin : public plugin::Plugin {
 public:
  static const plugin::PluginInfo& static_info();
  const plugin::PluginInfo& info() const override { return static_info(); }
  bool on_load(const plugin::PluginContext& ctx, std::string* error) override;
  void on_register(plugin::Registrar& reg) override;
  void on_play_begin(runtime::World& world) override;
  void on_play_end() override;
  void on_unload() override;
  ~StoryPlugin() override;

  static StoryPlugin* instance();
  // Feature plugins switch themselves on / off here (load / unload).
  void set_feature(const std::string& id, bool on);
  story::StoryFeatures features() const { return features_; }
  story::StoryRuntime* runtime_for(const runtime::World& w);
  story::StoryRuntime** slot() { return &current_; }
  const std::string& project_dir() const { return project_dir_; }
  const std::string& host() const { return host_; }

  // Editor panels (dialogue editor, bounty board, strongbox) install
  // through this, like the other built-ins.
  using EditorHook = std::function<void(const std::string& feature, plugin::Registrar&)>;
  static void set_editor_hook(EditorHook hook);
  static const EditorHook& editor_hook();

 private:
  std::map<const runtime::World*, std::unique_ptr<story::StoryRuntime>> worlds_;
  story::StoryRuntime* current_ = nullptr;
  story::StoryFeatures features_;
  std::string project_dir_;
  std::string host_;
};

// One class per feature; they only register their Lua table, asset type and
// editor panels and flip the feature on the core.
class StoryFeaturePlugin : public plugin::Plugin {
 public:
  bool on_load(const plugin::PluginContext& ctx, std::string* error) override;
  void on_register(plugin::Registrar& reg) override;
  void on_unload() override;

 protected:
  virtual const char* feature() const = 0;
  virtual const char* lua_name() const = 0;
  virtual void push_lua(lua_State* L, story::StoryRuntime** slot) const = 0;
  virtual plugin::AssetType asset() const { return {}; }
  std::string host_;
};

class QuestsPlugin final : public StoryFeaturePlugin {
 public:
  static const plugin::PluginInfo& static_info();
  const plugin::PluginInfo& info() const override { return static_info(); }

 protected:
  const char* feature() const override { return "quests"; }
  const char* lua_name() const override { return "quests"; }
  void push_lua(lua_State* L, story::StoryRuntime** slot) const override { story::push_quests_lua(L, slot); }
  plugin::AssetType asset() const override { return {".quest.json", "Quest"}; }
};

class DialoguePlugin final : public StoryFeaturePlugin {
 public:
  static const plugin::PluginInfo& static_info();
  const plugin::PluginInfo& info() const override { return static_info(); }

 protected:
  const char* feature() const override { return "dialogue"; }
  const char* lua_name() const override { return "dialogue"; }
  void push_lua(lua_State* L, story::StoryRuntime** slot) const override { story::push_dialogue_lua(L, slot); }
  plugin::AssetType asset() const override { return {".dlg.json", "Dialogue"}; }
};

class SavesPlugin final : public StoryFeaturePlugin {
 public:
  static const plugin::PluginInfo& static_info();
  const plugin::PluginInfo& info() const override { return static_info(); }

 protected:
  const char* feature() const override { return "saves"; }
  const char* lua_name() const override { return "saves"; }
  void push_lua(lua_State* L, story::StoryRuntime** slot) const override { story::push_saves_lua(L, slot); }
  plugin::AssetType asset() const override { return {".save.json", "Save slot"}; }
};

// Which story features a project enables (by plugin id), for hosts that
// attach the 3D extension.
story::StoryFeatures story_features(const std::vector<std::string>& enabled_plugin_ids);

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
