#pragma once

// Editor panels of the story plugins (Dusk theme), installed through
// StoryPlugin::set_editor_hook by register_editor_builtin_plugins:
//   Parley        dialogue/*.dlg.json node graph: drag nodes, links for
//                 next / choices, node + choice inspector, speakers, lint,
//                 an in-panel preview runner, the live line during Play.
//   Bounty Board  quests/*.quest.json: fields, objectives, rewards, lint;
//                 live status + start / complete / fail during Play.
//   Strongbox     save slots on disk; save / autosave / load during Play;
//                 live story variables.

#include "story/Dialogue.h"
#include "story/Quest.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

struct DialoguePanelState {
  std::string project_dir;
  std::vector<std::string> files;
  std::string path;
  story::Dialogue dlg;
  bool dirty = false;
  int selected = -1;
  int dragging = -1;
  float pan[2] = {40, 40};
  std::string status;
  std::string new_name = "townsfolk";
  std::vector<std::string> lint;
  double scanned = -1.0;
  // Preview runner.
  bool preview = false;
  story::Vars preview_vars;
  story::DialogueRunner preview_runner;
};
void draw_dialogue_panel(DialoguePanelState& s);
bool open_dialogue_file(DialoguePanelState& s, const std::string& rel);
bool save_dialogue_file(DialoguePanelState& s);

struct QuestPanelState {
  std::string project_dir;
  std::vector<std::string> files;
  std::string path;
  std::vector<story::QuestDef> quests;  // the open file
  int selected = 0;
  bool dirty = false;
  std::string status;
  std::string new_name = "bounty";
  std::vector<std::string> lint;
  double scanned = -1.0;
};
void draw_quest_panel(QuestPanelState& s);
bool open_quest_file(QuestPanelState& s, const std::string& rel);
bool save_quest_file(QuestPanelState& s);

struct SavesPanelState {
  std::string project_dir;
  std::string slot = "1";
  std::string status;
  double scanned = -1.0;
};
void draw_saves_panel(SavesPanelState& s);

void install_story_panels();

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
