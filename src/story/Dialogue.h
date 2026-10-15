#pragma once

// Parley (ts.dialogue) core: branching dialogue from
// <project>/dialogue/*.dlg.json and the DialogueRunner that walks it.
//
//   {"id": "hank", "start": "hello",
//    "speakers": {"hank": {"name": "Old Hank", "portrait": "assets/hank.png"}},
//    "nodes": [
//      {"id": "hello", "speaker": "hank", "text": "Evenin', {player}.",
//       "actions": ["set met_hank true"],
//       "choices": [
//         {"text": "Need a hand?", "next": "offer", "if": "quest:bell == inactive"},
//         {"text": "Bell's rung.", "next": "thanks", "if": "quest:bell == active && obj:bell/ring >= 1"},
//         {"text": "Just passing.", "next": "", "once": true}]},
//      {"id": "offer", "speaker": "hank", "text": "...", "next": "hello",
//       "x": 320, "y": 40}]}
//
// A node shows its text, runs its actions on entry, then offers the
// choices whose "if" holds (and, for "once", that were not taken) or, with
// no choices, continues to "next". Empty "next" ends the conversation.
// Text takes {var} from the story variables. x / y place a node in the
// editor panel's graph.

#include "scene/JsonTree.h"
#include "story/Vars.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace story {

struct DialogueChoice {
  std::string text;
  std::string next;
  std::string condition;  // "if"
  std::vector<std::string> actions;
  bool once = false;
};

struct DialogueNode {
  std::string id;
  std::string speaker;
  std::string text;
  std::string condition;  // "if": entering a failing node skips to next
  std::vector<std::string> actions;
  std::vector<DialogueChoice> choices;
  std::string next;
  float x = 0, y = 0;  // editor layout
};

struct Speaker {
  std::string name;
  std::string portrait;
};

struct Dialogue {
  std::string id;
  std::string start;
  std::map<std::string, Speaker> speakers;
  std::vector<DialogueNode> nodes;
  std::string file;

  const DialogueNode* node(const std::string& id) const;
  DialogueNode* node(const std::string& id);
};

bool parse_dialogue(const std::string& text, const std::string& file, Dialogue* out, std::string* error);
json::Value dialogue_to_json(const Dialogue& d);
// Missing targets / start, duplicate ids, unreachable nodes, bad
// conditions and actions, nodes that are dead ends without saying so.
std::vector<std::string> lint_dialogue(const Dialogue& d);

class DialogueRunner {
 public:
  struct Line {
    std::string node;
    std::string speaker;   // display name
    std::string portrait;
    std::string text;      // formatted
    std::vector<std::string> choices;  // formatted, visible only
    std::vector<int> choice_index;     // into node.choices
  };

  // Library: every *.dlg.json under <project>/dialogue (by "id", falling
  // back to the file stem).
  int load_dir(const std::string& project_dir, std::string* errors = nullptr);
  void add(Dialogue d);
  const Dialogue* find(const std::string& id) const;
  std::vector<std::string> ids() const;

  // Conditions and actions: the runtime wires these.
  ConditionEnv env;
  std::function<void(const std::string& action)> run_action;
  Vars* vars = nullptr;  // {var} text

  bool start(const std::string& id, const std::string& at_node = {}, std::string* error = nullptr);
  bool active() const { return !dlg_.empty(); }
  const std::string& dialogue_id() const { return dlg_; }
  const Line& line() const { return line_; }
  // Pick a visible choice (0-based) / continue a line without choices.
  bool choose(int visible_index);
  bool advance();
  void stop();
  // Ended since the last take (the dialogue id), for on_dialogue_end.
  std::vector<std::string> take_ended();

  // Choices taken (for "once"), saved with the game.
  json::Value to_json() const;
  void from_json(const json::Value& v);
  std::uint64_t lines_shown() const { return lines_shown_; }

 private:
  bool enter(const std::string& node, int depth);
  void finish();
  std::string seen_key(const std::string& node, int choice) const;

  std::map<std::string, Dialogue> lib_;
  std::string dlg_;
  Line line_;
  std::set<std::string> taken_;
  std::vector<std::string> ended_;
  std::uint64_t lines_shown_ = 0;
};

}  // namespace story
}  // namespace tombstone
}  // namespace ts
