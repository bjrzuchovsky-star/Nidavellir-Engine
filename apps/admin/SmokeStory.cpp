// --smoke-story: the story pieces (variables, quests, dialogue, saves) and
// the optional plugins riding a 2D world and a 3D world.

#include "SmokeStory.h"

#include "editor/ProjectInfo.h"
#include "editor/editor3d/Editor3DScreen.h"
#include "editor/plugins/EditorPlugins.h"
#include "editor/plugins/PluginPanelHost.h"
#include "editor/plugins/StoryPanels.h"
#include "editor/screens/Editor2DScreen.h"
#include "plugin/Catalog.h"
#include "plugin/PluginManager.h"
#include "plugins/Builtins.h"
#include "plugins/story/StoryPlugins.h"
#include "render3d/Scene3D.h"
#include "runtime/World.h"
#include "scene/SceneData.h"
#include "runtime/Input.h"
#include "story/Dialogue.h"
#include "story/Quest.h"
#include "story/SaveGame.h"
#include "story/StoryRuntime.h"
#include "story/Vars.h"
#include "story3d/StoryWorld3D.h"
#include "ui/Ui.h"
#include "ui/UiScript.h"
#include "world3d/World3D.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace story = ts::tombstone::story;
using namespace ts::tombstone;

namespace {

int fail(const std::string& m) {
  std::cerr << "[smoke] story FAIL: " << m << "\n";
  return 1;
}

void write_sample_files(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir / "quests", ec);
  fs::create_directories(dir / "dialogue", ec);
  fs::create_directories(dir / "scripts", ec);
  std::ofstream(dir / "quests" / "bell.quest.json") << R"({
    "id": "bell", "title": "Ring the Bell", "giver": "Old Hank",
    "description": "The church bell has gone quiet.",
    "objectives": [{"id": "ring", "text": "Ring the church bell", "count": 1}],
    "rewards": ["add gold 25", "event bounty_paid 25"], "next": "lanterns"})";
  std::ofstream(dir / "quests" / "lanterns.quest.json") << R"({
    "id": "lanterns", "title": "Light the Lanterns", "giver": "Old Hank",
    "requires": ["bell"], "auto_complete": false,
    "objectives": [{"id": "lit", "text": "Light lanterns", "count": 3},
                   {"id": "polish", "text": "Polish one", "optional": true, "count": 1}],
    "rewards": ["set badge true"]})";
  std::ofstream(dir / "dialogue" / "hank.dlg.json") << R"({
    "id": "hank", "start": "hello",
    "speakers": {"hank": {"name": "Old Hank"}},
    "nodes": [
      {"id": "hello", "speaker": "hank", "text": "Evenin', {player}.", "x": 0, "y": 0,
       "choices": [
         {"text": "Need a hand?", "next": "offer", "if": "quest:bell == inactive"},
         {"text": "The bell's rung.", "next": "thanks", "if": "quest:bell == active && obj:bell/ring >= 1"},
         {"text": "Just passing.", "next": "", "once": true}]},
      {"id": "offer", "speaker": "hank", "text": "Ring the church bell for me.", "next": "hello",
       "actions": ["quest start bell", "set met_hank true"], "x": 300, "y": 0},
      {"id": "thanks", "speaker": "hank", "text": "That's {gold} dollars' worth of quiet.", "next": "",
       "actions": ["quest progress bell ring"], "x": 300, "y": 180}]})";
}

// ---- Core pieces -------------------------------------------------------------------

int core_checks() {
  story::Vars v;
  v.set("gold", story::VarValue::number(25));
  v.set("player", story::VarValue::string("Rider"));
  v.set("met_hank", story::VarValue::boolean(true));
  if (v.format("{player} has {gold} dollars and {unknown}") != "Rider has 25 dollars and {unknown}") {
    return fail("vars format");
  }
  story::ConditionEnv env;
  env.vars = &v;
  env.quest_status = [](const std::string& q) { return q == "bell" ? std::string("active") : std::string("inactive"); };
  env.objective = [](const std::string& q, const std::string& o) { return q == "bell" && o == "ring" ? 1 : 0; };
  auto expect = [&](const std::string& c, bool want) {
    std::string e;
    if (!story::check_condition(c, &e)) return fail(c + " should parse: " + e);
    if (story::eval_condition(c, env) != want) return fail("\"" + c + "\" expected " + std::to_string(want));
    return 0;
  };
  if (expect("met_hank && gold >= 10", true) || expect("!met_hank || gold < 10", false) ||
      expect("quest:bell == active && obj:bell/ring >= 1", true) ||
      expect("quest:other == inactive", true) || expect("player == 'Rider'", true) ||
      expect("gold != 25", false)) {
    return 1;
  }
  std::string err;
  if (story::check_condition("gold === 3", &err)) return fail("bad operator should fail lint");
  story::Action a;
  if (!story::parse_action("quest progress bell ring 2", &a, &err) || a.args.size() != 4) return fail("parse action");
  if (story::parse_action("set only", &a, &err)) return fail("short action should fail");

  story::QuestLog log;
  std::vector<story::QuestDef> qs;
  const std::string bell = R"({"id":"bell","title":"Ring","objectives":[{"id":"ring","text":"Ring","count":2}],
    "rewards":["add gold 25"],"next":"next"})";
  const std::string nxt = R"({"id":"next","requires":["bell"],"objectives":[{"id":"a","text":"A"},{"id":"b","text":"B","optional":true}]})";
  if (!story::parse_quests(bell, "b.quest.json", &qs, &err) || !story::parse_quests(nxt, "n.quest.json", &qs, &err)) {
    return fail(err);
  }
  if (!story::lint_quests(qs).empty()) return fail("lint of good quests");
  for (auto& q : qs) log.add(std::move(q));
  std::string why;
  if (log.start("next", &why)) return fail("prerequisite should block start");
  if (!log.start("bell") || log.status("bell") != story::QuestStatus::Active) return fail("start");
  if (!log.progress("bell", "ring", 1) || log.objective_done("bell", "ring")) return fail("partial progress");
  std::vector<std::string> rewards;
  log.run_action = [&](const std::string& r) { rewards.push_back(r); };
  if (!log.progress("bell", "ring", 1) || log.status("bell") != story::QuestStatus::Completed) return fail("auto complete");
  if (rewards != std::vector<std::string>{"add gold 25"}) return fail("reward not run");
  if (log.status("next") != story::QuestStatus::Active) return fail("next quest should chain");
  const auto ev = log.take_events();
  int started = 0, done = 0;
  for (const auto& e : ev) {
    started += e.kind == story::QuestLog::Event::Kind::Started;
    done += e.kind == story::QuestLog::Event::Kind::Completed;
  }
  if (started != 2 || done != 1) return fail("events");
  const json::Value saved = log.to_json();
  story::QuestLog back;
  for (const story::QuestDef* d : log.defs()) back.add(*d);
  back.from_json(saved);
  if (back.status("bell") != story::QuestStatus::Completed || back.objective_progress("bell", "ring") != 2) {
    return fail("quest log round trip");
  }
  std::cout << "[smoke] story core OK (conditions, quest chain + rewards, round trip)\n";
  return 0;
}

int dialogue_checks() {
  story::Dialogue d;
  std::string err;
  const std::string text = R"({"id":"t","start":"a","speakers":{"n":{"name":"N"}},"nodes":[
    {"id":"a","speaker":"n","text":"Hi {name}","choices":[
      {"text":"yes","next":"b","if":"gold >= 10","actions":["set said_yes true"]},
      {"text":"no","next":"","once":true}]},
    {"id":"b","speaker":"n","text":"good","next":"c","actions":["add gold 5"]},
    {"id":"c","speaker":"n","text":"bye","next":"","if":"said_yes"},
    {"id":"d","speaker":"n","text":"hidden","next":""}]})";
  if (!story::parse_dialogue(text, "t.dlg.json", &d, &err)) return fail(err);
  const auto lint = story::lint_dialogue(d);
  if (lint.size() != 1 || lint[0].find("unreachable") == std::string::npos) return fail("lint should flag d");
  const std::string back = json::write(story::dialogue_to_json(d));
  story::Dialogue d2;
  if (!story::parse_dialogue(back, "t.dlg.json", &d2, &err) || d2.nodes.size() != 4) return fail("dialogue round trip");

  story::Vars vars;
  vars.set("name", story::VarValue::string("Rider"));
  vars.set("gold", story::VarValue::number(25));
  story::DialogueRunner r;
  r.add(d);
  r.vars = &vars;
  r.env.vars = &vars;
  r.run_action = [&](const std::string& a) {
    story::Action x;
    if (!story::parse_action(a, &x, nullptr)) return;
    if (x.verb == "set") vars.set(x.args[0], story::VarValue::parse(x.args[1]));
    else if (x.verb == "add") vars.add(x.args[0], story::VarValue::parse(x.args[1]).num());
  };
  if (!r.start("t") || r.line().text != "Hi Rider" || r.line().choices.size() != 2) return fail("start line");
  if (!r.choose(0) || r.line().node != "b" || !vars.get("said_yes").truthy()) return fail("choice action");
  if (!r.advance() || r.line().node != "c" || vars.get("gold").num() != 30) return fail("advance + add");
  if (!r.advance() || r.active()) return fail("should end");
  if (r.take_ended() != std::vector<std::string>{"t"}) return fail("ended event");
  // "once" and a failing condition hide choices.
  vars.set("gold", story::VarValue::number(0));
  if (!r.start("t") || r.line().choices != std::vector<std::string>{"no"}) return fail("condition hides yes");
  if (!r.choose(0) || r.active()) return fail("no ends the talk");
  if (!r.start("t") || !r.line().choices.empty()) return fail("once hides no the second time");
  if (r.choose(0) || !r.active()) return fail("choose out of range");
  r.advance();
  if (r.active()) return fail("empty next ends");
  std::cout << "[smoke] story dialogue OK (branches, once, conditions, actions, {var}, lint, round trip)\n";
  return 0;
}

int save_checks(const fs::path& dir) {
  story::Vars vars;
  story::SaveStore store(dir.string());
  store.scene = "Smoke";
  store.playtime = 12.5;
  store.autosave_slots = 2;
  store.provide({"vars", [&] { return vars.to_json(); }, [&](const json::Value& v) { vars.from_json(v); }});
  vars.set("gold", story::VarValue::number(25));
  std::string err;
  if (store.save("bad/slot", "", &err)) return fail("bad slot name should fail");
  if (!store.save("1", "Before the bell", &err)) return fail(err);
  vars.set("gold", story::VarValue::number(0));
  if (!store.load("1", &err) || vars.get("gold").num() != 25) return fail("load vars");
  if (!store.autosave() || !store.autosave() || !store.autosave()) return fail("autosave");
  const auto slots = store.list();
  if (slots.size() != 3) return fail("rotation should keep 2 autosaves + slot 1, got " + std::to_string(slots.size()));
  if (slots.front().slot.rfind("auto-", 0) != 0) return fail("newest should be an autosave");
  // Damaged slot.
  std::ofstream(fs::path(store.slots_dir()) / "2.save.json") << "{ not json";
  if (store.load("2", &err)) return fail("damaged slot should fail");
  // Missing section loads as empty (an older save).
  vars.set("gold", story::VarValue::number(1));
  if (!store.save("3", "", &err)) return fail(err);
  store.provide({"extra", [] { return json::Value::object(); }, [&](const json::Value& v) {
                   if (!v.is_object()) vars.set("bad", story::VarValue::boolean(true));
                 }});
  if (!store.load("3", &err) || vars.has("bad")) return fail("missing section");
  std::cout << "[smoke] story saves OK (" << slots.size() << " slots, rotation, damaged slot, missing section)\n";
  return 0;
}

// ---- Runtime + plugins -----------------------------------------------------------------

int runtime_checks(const fs::path& dir) {
  write_sample_files(dir);
  ui::UiSystem ui;  // documents + widget state only (nothing drawn)
  story::StoryRuntime rt;
  story::StoryHost h;
  h.project_dir = dir.string();
  h.scene_name = "Smoke";
  std::vector<runtime::UiCommand> cmds;
  h.ui = [&](runtime::UiCommand c) { cmds.push_back(std::move(c)); };
  std::vector<std::pair<std::string, std::string>> hooks;
  h.broadcast = [&](const char* name, const std::string& text, double) { hooks.emplace_back(name, text); };
  story::StoryFeatures f;
  f.quests = f.dialogue = f.saves = true;
  rt.start(std::move(h), f);
  ui::apply_ui_commands(ui, cmds);
  cmds.clear();
  if (!ui.has("dialogue") || !ui.has("journal") || !ui.has("quest_tracker") || !ui.has("save_menu")) {
    return fail("built-in UI documents");
  }
  if (!rt.dialogue.start("hank")) return fail("hank dialogue");
  rt.tick(0.016);
  ui::apply_ui_commands(ui, cmds);
  cmds.clear();
  if (!ui.visible("dialogue")) return fail("dialogue box should open");
  const ui::Widget* speaker = ui.find("dialogue.dlg_speaker");
  if (!speaker || speaker->text != "Old Hank") return fail("speaker label");
  // Choice 1: "Need a hand?" -> offer starts the quest.
  if (!rt.ui_event("dlg_choice_1", false, 0, "")) return fail("choice event");
  rt.tick(0.016);
  ui::apply_ui_commands(ui, cmds);
  cmds.clear();
  if (rt.quests.status("bell") != story::QuestStatus::Active) return fail("dialogue should start bell");
  bool started = false;
  for (const auto& [n, t] : hooks) started |= n == "on_quest_started" && t == "bell";
  if (!started) return fail("on_quest_started hook");
  if (!ui.visible("quest_tracker")) return fail("tracker should show the active bounty");
  // Finish the talk through the box: Continue -> hello, "Just passing." ends it.
  if (!rt.ui_event("dlg_next", false, 0, "") || rt.dialogue.line().node != "hello") return fail("continue");
  if (rt.dialogue.line().choices != std::vector<std::string>{"Just passing."}) return fail("hello choices mid-quest");
  rt.ui_event("dlg_choice_1", false, 0, "");
  rt.tick(0.016);
  ui::apply_ui_commands(ui, cmds);
  cmds.clear();
  if (rt.dialogue.active() || ui.visible("dialogue")) return fail("dialogue box should close");
  rt.quests.progress("bell", "ring");
  rt.tick(0.016);
  bool done = false, saved = false;
  for (const auto& [n, t] : hooks) {
    done |= n == "on_quest_completed" && t == "bell";
    saved |= n == "on_game_saved";
  }
  if (!done || !saved) return fail("completion hook + autosave on quest");
  if (rt.quests.status("lanterns") != story::QuestStatus::Active) return fail("chain in runtime");
  if (rt.vars.get("gold").num() != 25) return fail("reward in runtime");
  // Save, wipe, load.
  std::string err;
  if (!rt.save("1", "test", &err)) return fail(err);
  rt.vars.set("gold", story::VarValue::number(0));
  rt.quests.reset();
  if (!rt.load("1", &err) || rt.vars.get("gold").num() != 25 ||
      rt.quests.status("bell") != story::QuestStatus::Completed) {
    return fail("runtime load");
  }
  rt.tick(0.016);
  bool loaded = false;
  for (const auto& [n, t] : hooks) loaded |= n == "on_game_loaded" && t == "1";
  if (!loaded) return fail("on_game_loaded");
  // Journal UI.
  rt.show_journal(true);
  rt.tick(0.016);
  ui::apply_ui_commands(ui, cmds);
  cmds.clear();
  if (!ui.visible("journal")) return fail("journal");
  const ui::Widget* list = ui.find("journal.jr_list");
  if (!list || list->items.size() < 2) return fail("journal list");
  std::cout << "[smoke] story runtime OK (dialogue UI, quest chain, rewards, hooks, tracker, journal, save/load)\n";
  return 0;
}

std::string text_of(const runtime::ScriptWorld& w, std::uint64_t id, const char* key) {
  const ScriptValue* v = w.script_get(id, key);
  return v && v->type == ScriptValue::Type::Text ? v->text : std::string();
}

int plugin_checks(const fs::path& dir) {
  write_sample_files(dir);
  plugin::Catalog cat;
  plugins::register_builtin_plugins(cat, nullptr);
  if (!cat.find("ts.story") || !cat.find("ts.quests") || !cat.find("ts.dialogue") || !cat.find("ts.saves")) {
    return fail("story plugins should be built-ins");
  }
  plugin::PluginManager m(cat);
  plugin::PluginContext ctx;
  ctx.project_dir = dir.string();
  // A feature never rides without the core.
  m.load({"ts.quests"}, ctx);
  if (m.loaded("ts.quests") && !m.loaded("ts.story")) return fail("ts.quests loaded without ts.story");
  m.unload_all();
  m.load({"ts.story", "ts.quests", "ts.dialogue", "ts.saves"}, ctx);
  if (!m.loaded("ts.story") || !m.loaded("ts.quests") || !m.loaded("ts.dialogue") || !m.loaded("ts.saves")) {
    return fail("the four story plugins should load together");
  }
  runtime::World w;
  w.set_extensions(m.world_extensions());
  std::vector<Entity2D> ents;
  Entity2D town;
  town.id = 1;
  town.name = "Town";
  town.w = town.h = 16;
  town.script = ScriptData{};
  town.script->path = "scripts/town.lua";
  ents.push_back(town);
  Entity2D rider;
  rider.id = 2;
  rider.name = "Rider";
  rider.x = 100;
  rider.y = 200;
  rider.w = rider.h = 24;
  rider.player = PlayerControllerData{};
  ents.push_back(rider);
  w.script_library().put("scripts/town.lua", R"(
    local asked = false
    function on_start()
      story.set("player", "Rider")
      dialogue.start("hank")
    end
    function on_tick()
      if not asked and dialogue.active() == "hank" then
        asked = true
        dialogue.choose(1)
      end
      if quests.status("bell") == "active" and quests.count("bell", "ring") == 0 then
        quests.progress("bell", "ring")
      end
    end
    function on_quest_completed(id) self:set("done", id) end
    function on_game_loaded(slot) self:set("loaded", slot) end
  )");
  std::string err;
  if (!w.build(ents, dir.string(), &err)) return fail("world build: " + err);
  m.play_begin(w);
  runtime::InputFrame in;
  for (int i = 0; i < 6; ++i) w.step(in);
  if (text_of(w, 1, "done") != "bell") return fail("2D ride: the quest should complete through dialogue + hooks");
  plugins::StoryPlugin* core = plugins::StoryPlugin::instance();
  story::StoryRuntime* rt = core ? core->runtime_for(w) : nullptr;
  if (!rt || rt->vars.get("gold").num() != 25) return fail("2D reward");
  // UI went out through the world's queue.
  ui::UiSystem ui;
  ui::apply_ui_commands(ui, w.take_ui_commands());
  if (!ui.has("dialogue") || !ui.has("quest_tracker")) return fail("2D built-in UI documents");
  if (!rt->save("2d", "", &err)) return fail(err);
  const runtime::Actor* a = w.find(2);
  const float x0 = a ? a->data.x : 0.0f;
  w.script_move(2, x0 + 50, a ? a->data.y : 0.0f);
  rt->vars.set("gold", story::VarValue::number(0));
  if (!rt->load("2d", &err)) return fail(err);
  for (int i = 0; i < 2; ++i) w.step(in);
  if (text_of(w, 1, "loaded") != "2d") return fail("on_game_loaded hook in 2D");
  if (rt->vars.get("gold").num() != 25) return fail("2D load vars");
  a = w.find(2);
  if (!a || std::fabs(a->data.x - x0) > 1.0f) return fail("2D player position restored");
  m.play_end();
  w.clear();
  m.unload_all();
  std::cout << "[smoke] story plugins OK (2D ride: dialogue starts a quest, hooks, reward, UI, save/load, rider)\n";
  return 0;
}

// The shipped Ghost Town demo as generated (no smoke data): walk to Old Hank,
// interact, take the job, ring the bell, get paid, autosave.
int ghost_town_demo_checks(const fs::path& dir) {
  std::string err;
  if (!scene3d::write_ghost_town_sample(dir.string(), "Story Demo", &err)) return fail(err);
  scene3d::Scene3D scene;
  if (!scene3d::load_file(scene3d::scene_path(dir.string()), &scene, &err)) return fail(err);
  render3d::Vec3 hank{}, bell{};
  bool have_hank = false, have_bell = false;
  for (const auto& e : scene.entities) {
    if (e.script && e.script->path == "scripts/hank.lua") { hank = e.transform.position; have_hank = true; }
    if (e.script && e.script->path == "scripts/bell.lua") { bell = e.transform.position; have_bell = true; }
  }
  if (!have_hank || !have_bell) return fail("demo: Hank / bell missing");
  world3d::World3D world;
  story::StoryFeatures f;
  f.quests = f.dialogue = f.saves = true;
  story3d::attach(world, f);
  if (!world.start(scene, dir.string(), &err)) return fail("demo start: " + err);
  world.run_ticks(4, {});
  story3d::StoryWorld3D* st = story3d::find(world);
  if (!st) return fail("demo: no story");
  story::StoryRuntime& rt = st->runtime();
  world3d::Input3D press;
  press.action = true;
  if (!world.set_rider_position({hank.x + 0.8f, hank.y, hank.z})) return fail("demo: move rider");
  world.run_ticks(2, {});
  world.run_ticks(1, press);
  world.run_ticks(2, {});
  if (!rt.dialogue.active() || rt.dialogue.dialogue_id() != "hank") return fail("demo: Hank should talk on interact");
  if (!rt.dialogue.choose(0)) return fail("demo: take the job");
  world.run_ticks(2, {});
  if (rt.quests.status("bell") != story::QuestStatus::Active) return fail("demo: bell quest not started");
  rt.dialogue.advance();
  world.run_ticks(2, {});
  rt.quests.progress("bell", "ring");  // what bell.lua's trigger does
  world.run_ticks(4, {});
  if (rt.quests.status("bell") != story::QuestStatus::Completed) return fail("demo: bell not completed");
  if (rt.vars.get("gold").num() != 25) return fail("demo: reward");
  world.run_ticks(1, press);
  world.run_ticks(2, {});
  if (!rt.dialogue.active()) return fail("demo: second chat");
  world.stop();
  std::cout << "[smoke] story demo OK (Ghost Town: interact with Old Hank, take Ring the Bell, ring it, $25)\n";
  return 0;
}

int world3d_checks(const fs::path& dir) {
  std::string err;
  if (!scene3d::write_ghost_town_sample(dir.string(), "Story 3D", &err)) return fail(err);
  write_sample_files(dir);
  std::ofstream(dir / "scripts" / "story.lua") << R"(
    function on_start()
      story.set("player", "Rider")
      dialogue.start("hank")
      dialogue.choose(1)
    end
    function on_quest_completed(id)
      self:set("done", id)
      saves.save("3d", "after the bell")
    end
  )";
  // The bell rings itself when the quest starts (the smoke's stand-in for
  // walking there): a small script on the Bell entity is added below.
  scene3d::Scene3D scene;
  if (!scene3d::load_file(scene3d::scene_path(dir.string()), &scene, &err)) return fail(err);
  scene3d::Entity3D story_e;
  story_e.id = 90001;
  story_e.name = "Story";
  story_e.script = ScriptData{};
  story_e.script->path = "scripts/story.lua";
  scene.entities.push_back(story_e);
  world3d::World3D world;
  story::StoryFeatures f;
  f.quests = f.dialogue = f.saves = true;
  story3d::attach(world, f);
  if (!world.start(scene, dir.string(), &err)) return fail("3D start: " + err);
  world.run_ticks(6, {});
  story3d::StoryWorld3D* st = story3d::find(world);
  if (!st || st->runtime().quests.status("bell") != story::QuestStatus::Active) return fail("3D quest not started");
  st->runtime().quests.progress("bell", "ring");
  world.run_ticks(4, {});
  if (st->runtime().quests.status("bell") != story::QuestStatus::Completed) return fail("3D completion");
  if (st->runtime().vars.get("gold").num() != 25) return fail("3D reward");
  if (!st->runtime().saves.exists("3d")) return fail("3D save on completion");
  if (text_of(world, 90001, "done") != "bell") return fail("3D on_quest_completed hook");
  // Move the rider, reload: back where the save was.
  render3d::Vec3 pos;
  if (!world.rider_position(&pos)) return fail("no rider");
  const render3d::Vec3 saved = pos;
  if (!world.set_rider_position({saved.x + 5, saved.y, saved.z + 5})) return fail("move rider");
  world.run_ticks(2, {});
  if (!st->runtime().load("3d", &err)) return fail(err);
  world.run_ticks(2, {});
  if (!world.rider_position(&pos) || std::fabs(pos.x - saved.x) > 0.05f || std::fabs(pos.z - saved.z) > 0.05f) {
    return fail("3D rider position not restored");
  }
  world.stop();
  std::cout << "[smoke] story 3D OK (dialogue starts a quest, reward, hook, save, rider restored)\n";
  return 0;
}

int editor_checks(const fs::path& dir) {
  std::string err;
  if (!scene3d::write_ghost_town_sample((dir / "p3d").string(), "Story Editor", &err)) return fail(err);
  write_sample_files(dir / "p3d");
  editor::ProjectInfo info;
  info.id = "_story_editor";
  info.name = "Story Editor";
  info.kind = editor::ProjectKind::ThreeD;
  info.path = (dir / "p3d").string();
  info.plugins = {"ts.story", "ts.quests", "ts.dialogue", "ts.saves"};
  info.plugins_set = true;
  editor::Editor3DScreen e(info);
  e.on_enter();
  if (!e.plugins().loaded("ts.quests") || !e.plugins().loaded("ts.dialogue") || !e.plugins().loaded("ts.saves")) {
    return fail("editor: plugins should load");
  }
  int panels = 0;
  for (const auto& p : e.plugins().manager().editor_panels()) {
    panels += p.item.id == "story.parley" || p.item.id == "story.bounty_board" || p.item.id == "story.strongbox";
  }
  if (panels != 3) return fail("editor: the three story panels");
  if (!e.play_start()) return fail("editor: Play");
  // The panels open during Play (same path as the View menu).
  ImGuiContext* prev = ImGui::GetCurrentContext();
  ImGuiContext* c = ImGui::CreateContext();
  ImGui::SetCurrentContext(c);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(1280, 800);
  io.IniFilename = nullptr;
  unsigned char* px = nullptr;
  int fw = 0, fh = 0;
  io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
  std::map<std::string, bool> open;
  bool shown = false;
  for (int frame = 0; frame < 2; ++frame) {
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::Begin("View##probe");
    editor::draw_plugin_panel_menu_items(e.plugins(), open);
    ImGui::End();
    for (const char* id : {"story.parley", "story.bounty_board", "story.strongbox"}) open[id] = true;
    editor::draw_open_plugin_panels(e.plugins(), open);
    shown = ImGui::FindWindowByName("Parley###plugin_story.parley") &&
            ImGui::FindWindowByName("Parley###plugin_story.parley")->Active &&
            ImGui::FindWindowByName("Bounty Board###plugin_story.bounty_board") &&
            ImGui::FindWindowByName("Bounty Board###plugin_story.bounty_board")->Active &&
            ImGui::FindWindowByName("Strongbox###plugin_story.strongbox") &&
            ImGui::FindWindowByName("Strongbox###plugin_story.strongbox")->Active;
    ImGui::Render();
  }
  ImGui::DestroyContext(c);
  ImGui::SetCurrentContext(prev);
  if (!shown) return fail("editor: panels should draw during Play");
  // The panel opens the sample conversation and the lint is clean.
  editor::DialoguePanelState ds;
  ds.project_dir = info.path;
  if (!editor::open_dialogue_file(ds, "dialogue/hank.dlg.json") || !ds.lint.empty()) return fail("editor: open hank");
  editor::QuestPanelState qs;
  qs.project_dir = info.path;
  if (!editor::open_quest_file(qs, "quests/bell.quest.json") || !qs.lint.empty()) return fail("editor: open bell");
  e.play_stop();
  e.on_exit();
  std::cout << "[smoke] story editor OK (3 panels in View during Play, sample files lint clean)\n";
  return 0;
}

}  // namespace

int run_story_smoke() {
  const fs::path root = fs::absolute("./TombStoneProjects/_story_smoke");
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root / "scripts", ec);
  if (core_checks() != 0) return 1;
  if (dialogue_checks() != 0) return 1;
  if (save_checks(root / "saves_only") != 0) return 1;
  if (runtime_checks(root / "runtime") != 0) return 1;
  if (plugin_checks(root / "plugin") != 0) return 1;
  if (world3d_checks(root / "w3d") != 0) return 1;
  if (ghost_town_demo_checks(root / "demo") != 0) return 1;
  if (editor_checks(root / "editor") != 0) return 1;
  fs::remove_all(root, ec);
  std::cout << "[smoke] story OK\n";
  return 0;
}
