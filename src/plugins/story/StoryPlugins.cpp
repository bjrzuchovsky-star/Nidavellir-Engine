#include "plugins/story/StoryPlugins.h"

#include "plugin/Catalog.h"
#include "runtime/World.h"

#include <algorithm>

using ts::tombstone::runtime::World;

namespace ts {
namespace tombstone {
namespace plugins {

namespace {

StoryPlugin*& instance_ref() {
  static StoryPlugin* p = nullptr;
  return p;
}

StoryPlugin::EditorHook& hook_ref() {
  static StoryPlugin::EditorHook h;
  return h;
}

plugin::PluginInfo feature_info(const char* id, const char* name, const char* desc) {
  plugin::PluginInfo i;
  i.id = id;
  i.name = name;
  i.description = desc;
  i.author = "TombStone";
  i.version = plugin::Version{1, 0, 0};
  i.category = plugin::PluginCategory::Gameplay;
  i.project_types = plugin::kType2D | plugin::kType3D | plugin::kTypeVR;
  i.default_enabled = false;
  plugin::Dependency core;
  core.id = "ts.story";
  core.min = plugin::Version{1, 0, 0};
  i.deps.push_back(core);
  return i;
}

}  // namespace

// ---- ts.story ------------------------------------------------------------------

const plugin::PluginInfo& StoryPlugin::static_info() {
  static const plugin::PluginInfo info = [] {
    plugin::PluginInfo i;
    i.id = "ts.story";
    i.name = "Story Core";
    i.description =
        "Shared story variables (`story` Lua table), the condition / action language and the "
        "per-world runtime that Bounty Board, Parley and Strongbox plug into.";
    i.author = "TombStone";
    i.version = plugin::Version{1, 0, 0};
    i.category = plugin::PluginCategory::Gameplay;
    i.project_types = plugin::kType2D | plugin::kType3D | plugin::kTypeVR;
    i.default_enabled = false;
    return i;
  }();
  return info;
}

StoryPlugin::~StoryPlugin() {
  if (instance_ref() == this) instance_ref() = nullptr;
}

StoryPlugin* StoryPlugin::instance() { return instance_ref(); }
void StoryPlugin::set_editor_hook(EditorHook hook) { hook_ref() = std::move(hook); }
const StoryPlugin::EditorHook& StoryPlugin::editor_hook() { return hook_ref(); }

bool StoryPlugin::on_load(const plugin::PluginContext& ctx, std::string* /*error*/) {
  instance_ref() = this;
  project_dir_ = ctx.project_dir;
  host_ = ctx.host;
  features_ = {};
  return true;
}

void StoryPlugin::on_unload() {
  worlds_.clear();
  current_ = nullptr;
  if (instance_ref() == this) instance_ref() = nullptr;
}

void StoryPlugin::set_feature(const std::string& id, bool on) {
  if (id == "quests") features_.quests = on;
  else if (id == "dialogue") features_.dialogue = on;
  else if (id == "saves") features_.saves = on;
}

story::StoryRuntime* StoryPlugin::runtime_for(const World& w) {
  const auto it = worlds_.find(&w);
  return it == worlds_.end() ? nullptr : it->second.get();
}

void StoryPlugin::on_register(plugin::Registrar& reg) {
  reg.add_component("Story", "Story variables and the runtime quests / dialogue / saves share");
  reg.add_system("story.tick", [this](World& w, double dt) {
    if (story::StoryRuntime* r = runtime_for(w)) r->tick(dt);
  });
  reg.add_lua_api("story", [this](lua_State* L) { story::push_story_lua(L, &current_); });
  if (hook_ref() && host_.rfind("editor", 0) == 0) hook_ref()("story", reg);
}

void StoryPlugin::on_play_begin(World& world) {
  if (world.net_role() == runtime::NetRole::Client) return;
  auto rt = std::make_unique<story::StoryRuntime>();
  story::StoryHost h;
  h.project_dir = project_dir_;
  h.scene_name = "";
  World* w = &world;
  h.ui = [w](runtime::UiCommand c) { w->script_ui(std::move(c)); };
  h.broadcast = [w](const char* hook, const std::string& text, double n) { w->script_broadcast(hook, text, n); };
  h.toast = [w](const std::string& text, double seconds) { w->script_toast(-1, text, seconds, 0); };
  h.log = [w](int level, const std::string& text) {
    runtime::LogEntry e;
    e.level = level >= 2 ? runtime::LogLevel::Error : level == 1 ? runtime::LogLevel::Warn : runtime::LogLevel::Info;
    e.channel = "story";
    e.text = text;
    w->script_log(std::move(e));
  };
  h.get_player = [w](float p[3]) {
    const std::uint64_t id = w->script_player(0);
    const auto* e = id ? w->script_entity(id) : nullptr;
    if (!e) return false;
    p[0] = e->x;
    p[1] = e->y;
    p[2] = 0;
    return true;
  };
  h.set_player = [w](const float p[3]) {
    const std::uint64_t id = w->script_player(0);
    if (!id) return false;
    w->script_move(id, p[0], p[1]);
    return true;
  };
  story::StoryRuntime* raw = rt.get();
  world.set_ui_tap("ts.story", [raw](const std::string& wd, bool change, double v, const std::string& t) {
    return raw->ui_event(wd, change, v, t);
  });
  raw->start(std::move(h), features_);
  current_ = raw;
  worlds_[&world] = std::move(rt);
}

void StoryPlugin::on_play_end() {
  for (auto& [w, rt] : worlds_) rt->stop();
  worlds_.clear();
  current_ = nullptr;
}

// ---- Feature plugins ---------------------------------------------------------------

bool StoryFeaturePlugin::on_load(const plugin::PluginContext& ctx, std::string* error) {
  StoryPlugin* core = StoryPlugin::instance();
  if (!core) {
    if (error) *error = "needs ts.story (Story Core)";
    return false;
  }
  host_ = ctx.host;
  core->set_feature(feature(), true);
  return true;
}

void StoryFeaturePlugin::on_unload() {
  if (StoryPlugin* core = StoryPlugin::instance()) core->set_feature(feature(), false);
}

void StoryFeaturePlugin::on_register(plugin::Registrar& reg) {
  StoryPlugin* core = StoryPlugin::instance();
  story::StoryRuntime** slot = core ? core->slot() : nullptr;
  const plugin::AssetType a = asset();
  if (!a.extension.empty()) reg.add_asset_type(a);
  reg.add_lua_api(lua_name(), [this, slot](lua_State* L) { push_lua(L, slot); });
  if (StoryPlugin::editor_hook() && host_.rfind("editor", 0) == 0) StoryPlugin::editor_hook()(feature(), reg);
}

const plugin::PluginInfo& QuestsPlugin::static_info() {
  static const plugin::PluginInfo info = feature_info(
      "ts.quests", "Bounty Board (Quests)",
      "Quests and bounties: quests/*.quest.json with objectives, chains and rewards, a journal and "
      "tracker UI, the `quests` Lua table and on_quest_* script hooks.");
  return info;
}

const plugin::PluginInfo& DialoguePlugin::static_info() {
  static const plugin::PluginInfo info = feature_info(
      "ts.dialogue", "Parley (Dialogue)",
      "Branching dialogue/*.dlg.json with conditions, variables, actions and speakers, a dialogue "
      "box UI, the `dialogue` Lua table and a node editor panel.");
  return info;
}

const plugin::PluginInfo& SavesPlugin::static_info() {
  static const plugin::PluginInfo info = feature_info(
      "ts.saves", "Strongbox (Saves)",
      "Save slots and a rotating autosave: story variables, quests, dialogue memory and the rider's "
      "position, a slot menu UI and the `saves` Lua table.");
  return info;
}

story::StoryFeatures story_features(const std::vector<std::string>& ids) {
  story::StoryFeatures f;
  const bool core = std::find(ids.begin(), ids.end(), "ts.story") != ids.end();
  auto has = [&](const char* id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
  // The feature plugins pull the core in; a project naming only a feature
  // still gets it.
  (void)core;
  f.quests = has("ts.quests");
  f.dialogue = has("ts.dialogue");
  f.saves = has("ts.saves");
  return f;
}

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts

TS_PLUGIN_STATIC(story, ::ts::tombstone::plugins::StoryPlugin)
TS_PLUGIN_STATIC(quests, ::ts::tombstone::plugins::QuestsPlugin)
TS_PLUGIN_STATIC(dialogue, ::ts::tombstone::plugins::DialoguePlugin)
TS_PLUGIN_STATIC(saves, ::ts::tombstone::plugins::SavesPlugin)
