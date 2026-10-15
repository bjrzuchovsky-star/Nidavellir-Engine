#pragma once

// The story runtime one world rides with: shared variables plus whichever
// of quests (ts.quests), dialogue (ts.dialogue) and saves (ts.saves) the
// project enables. World-agnostic: the 2D plugin and the 3D extension
// (story3d) fill a StoryHost with the world's UI queue, script broadcast,
// toasts and rider position.
//
// UI: built-in Dusk documents "dialogue", "journal", "quest_tracker" and
// "save_menu" are added to the host's UiSystem unless the project ships
// its own ui/<name>.ui.json with the same widget ids. Script hooks
// (every running Lua script that defines them):
//   on_quest_started(id)  on_quest_completed(id)  on_quest_failed(id)
//   on_objective("quest/objective", count)
//   on_dialogue_event(name, number)  on_dialogue_end(dialogue_id)
//   on_game_saved(slot)  on_game_loaded(slot)
// Settings: optional <project>/story.json
//   {"autosave_seconds": 300, "autosave_on_quest": true, "autosave_slots": 3,
//    "tracker": true, "toasts": true}

#include "runtime/Script.h"
#include "story/Dialogue.h"
#include "story/Quest.h"
#include "story/SaveGame.h"
#include "story/Vars.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct lua_State;

namespace ts {
namespace tombstone {
namespace story {

struct StoryHost {
  std::string project_dir;
  std::string scene_name;
  std::function<void(runtime::UiCommand)> ui;
  std::function<void(const char* hook, const std::string& text, double number)> broadcast;
  std::function<void(const std::string& text, double seconds)> toast;
  std::function<void(int level, const std::string& text)> log;  // 0 info, 1 warn, 2 error
  std::function<bool(float xyz[3])> get_player;
  std::function<bool(const float xyz[3])> set_player;
};

struct StoryFeatures {
  bool quests = false;
  bool dialogue = false;
  bool saves = false;
};

struct StorySettings {
  double autosave_seconds = 300.0;  // 0 = only explicit / quest autosaves
  bool autosave_on_quest = true;
  int autosave_slots = 3;
  bool tracker = true;
  bool toasts = true;
};
StorySettings load_story_settings(const std::string& project_dir);

// The built-in documents (JSON text), also what the docs show.
std::string builtin_dialogue_ui();
std::string builtin_journal_ui();
std::string builtin_tracker_ui();
std::string builtin_save_menu_ui();
// Greedy word wrap at `width` characters ('\n' kept).
std::string wrap_text(const std::string& text, std::size_t width);

class StoryRuntime {
 public:
  StoryRuntime();
  ~StoryRuntime();
  StoryRuntime(const StoryRuntime&) = delete;
  StoryRuntime& operator=(const StoryRuntime&) = delete;

  void start(StoryHost host, StoryFeatures features);
  void stop();
  bool running() const { return running_; }
  void tick(double dt);
  // Widget events (the host's UI tap): true = one of ours, consumed.
  bool ui_event(const std::string& widget, bool change, double value, const std::string& text);

  const StoryFeatures& features() const { return features_; }
  StorySettings settings;
  Vars vars;
  QuestLog quests;
  DialogueRunner dialogue;
  SaveStore saves;
  const std::string& load_errors() const { return load_errors_; }

  // Actions (Vars.h): rewards, dialogue lines, scripts.
  void run_action(const std::string& action);

  // Saves.
  bool save(const std::string& slot, const std::string& label, std::string* error = nullptr);
  bool autosave(std::string* slot = nullptr, std::string* error = nullptr);
  bool load(const std::string& slot, std::string* error = nullptr);
  double playtime() const { return playtime_; }

  // UI.
  void show_journal(bool on);
  bool journal_open() const { return journal_open_; }
  void show_save_menu(bool on);
  bool save_menu_open() const { return save_menu_open_; }
  void dialogue_changed() { dlg_dirty_ = true; }
  void quests_changed() { quest_dirty_ = true; }
  const std::string& last_event() const { return last_event_; }
  const std::string& last_event_arg() const { return last_event_arg_; }

  // Counters (smokes, editor panel).
  std::uint64_t hooks_fired() const { return hooks_fired_; }
  std::uint64_t autosaves() const { return autosaves_; }
  std::uint64_t ui_posts() const { return ui_posts_; }

  // The runtime riding now (editor panels' live view); null when none.
  static StoryRuntime* latest();

 private:
  void post(runtime::UiCommand c);
  void ui_text(const std::string& id, const std::string& text);
  void ui_visible(const std::string& id, bool on);
  void ui_show(const std::string& doc, bool on);
  void refresh_dialogue();
  void refresh_tracker();
  void refresh_journal();
  void refresh_save_menu();
  void flush_events();
  void hook(const char* name, const std::string& text, double number);
  void say(const std::string& text, double seconds = 2.5);

  StoryHost host_;
  StoryFeatures features_;
  bool running_ = false;
  bool dlg_dirty_ = false, quest_dirty_ = false;
  bool dlg_shown_ = false;
  bool journal_open_ = false, save_menu_open_ = false;
  int journal_sel_ = 0;
  int save_sel_ = 0;
  std::vector<std::string> journal_ids_;
  std::vector<std::string> save_slots_;
  double playtime_ = 0.0;
  double since_autosave_ = 0.0;
  bool autosave_due_ = false;
  std::vector<std::pair<std::string, std::string>> pending_hooks_;  // saved / loaded
  std::vector<std::pair<std::string, double>> pending_events_;      // dialogue events
  std::string last_event_, last_event_arg_;
  std::string load_errors_;
  std::uint64_t hooks_fired_ = 0, autosaves_ = 0, ui_posts_ = 0;
};

// Lua tables over a runtime slot (the slot may be null between rides).
// story: get / set / add / format. quests / dialogue / saves: see docs.
void push_story_lua(lua_State* L, StoryRuntime** slot);
void push_quests_lua(lua_State* L, StoryRuntime** slot);
void push_dialogue_lua(lua_State* L, StoryRuntime** slot);
void push_saves_lua(lua_State* L, StoryRuntime** slot);

}  // namespace story
}  // namespace tombstone
}  // namespace ts
