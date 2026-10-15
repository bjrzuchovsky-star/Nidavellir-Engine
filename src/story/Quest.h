#pragma once

// Bounty Board (ts.quests) core: quest definitions from
// <project>/quests/*.quest.json and the live QuestLog.
//
//   {"id": "bell", "title": "Ring the Bell", "giver": "Old Hank",
//    "description": "...", "requires": ["intro"], "next": "sequel",
//    "objectives": [{"id": "ring", "text": "Ring the church bell", "count": 1},
//                   {"id": "lantern", "text": "...", "optional": true, "hidden": true}],
//    "rewards": ["add gold 25", "set badge true"], "auto_complete": true}
//
// A file holds one quest or {"quests": [...]}. A quest completes itself
// when every non-optional objective is done (auto_complete, the default);
// rewards are actions (see Vars.h), run through the host's action hook;
// "next" starts on completion. Every change is queued as an Event for the
// script hooks and the journal.

#include "scene/JsonTree.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace story {

struct ObjectiveDef {
  std::string id;
  std::string text;
  int count = 1;
  bool optional = false;
  bool hidden = false;  // not listed until it has progress
};

struct QuestDef {
  std::string id;
  std::string title;
  std::string description;
  std::string giver;
  std::vector<std::string> prereqs;  // "requires"
  std::vector<ObjectiveDef> objectives;
  std::vector<std::string> rewards;
  std::string next;
  bool auto_complete = true;
  std::string file;  // project-relative, for messages

  const ObjectiveDef* objective(const std::string& id) const;
};

bool parse_quests(const std::string& text, const std::string& file, std::vector<QuestDef>* out,
                  std::string* error);
json::Value quest_to_json(const QuestDef& q);
// Problems a designer should see (unknown requires / next, bad rewards,
// duplicate objective ids, counts < 1).
// `known`: quest ids defined elsewhere (other files) that links may name.
std::vector<std::string> lint_quests(const std::vector<QuestDef>& quests,
                                     const std::vector<std::string>& known = {});

enum class QuestStatus { Inactive, Active, Completed, Failed };
const char* quest_status_name(QuestStatus s);
bool quest_status_from(const std::string& s, QuestStatus* out);

class QuestLog {
 public:
  struct Entry {
    QuestStatus status = QuestStatus::Inactive;
    std::map<std::string, int> progress;
    std::uint64_t order = 0;  // start order (journal sort)
  };
  struct Event {
    enum class Kind { Started, Objective, Completed, Failed } kind = Kind::Started;
    std::string quest;
    std::string objective;
    int value = 0;
  };

  // Every *.quest.json under <project>/quests. Errors one line each.
  int load_dir(const std::string& project_dir, std::string* errors = nullptr);
  void add(QuestDef q);
  void clear_defs() { defs_.clear(); }
  const QuestDef* def(const std::string& id) const;
  std::vector<const QuestDef*> defs() const;

  // Rewards / other actions go out through this (the runtime runs them).
  std::function<void(const std::string& action)> run_action;

  bool start(const std::string& id, std::string* why = nullptr);
  // Adds n (clamped to the objective's count); auto-completes.
  bool progress(const std::string& quest, const std::string& objective, int n = 1);
  bool set_progress(const std::string& quest, const std::string& objective, int value);
  bool complete(const std::string& id);
  bool fail(const std::string& id);
  void reset();  // every quest back to inactive (defs stay)

  QuestStatus status(const std::string& id) const;
  int objective_progress(const std::string& quest, const std::string& objective) const;
  bool objective_done(const std::string& quest, const std::string& objective) const;
  bool ready(const std::string& id) const;  // every required objective done
  const Entry* entry(const std::string& id) const;
  // Active first (newest first), then completed, then failed.
  std::vector<std::string> journal_order() const;

  const std::string& tracked() const { return tracked_; }
  void track(const std::string& id) { tracked_ = id; }

  std::vector<Event> take_events();
  const std::vector<Event>& pending_events() const { return events_; }

  json::Value to_json() const;
  void from_json(const json::Value& v);

 private:
  Entry& ent(const std::string& id) { return state_[id]; }
  void check_done(const std::string& id);

  std::map<std::string, QuestDef> defs_;
  std::map<std::string, Entry> state_;
  std::vector<Event> events_;
  std::string tracked_;
  std::uint64_t next_order_ = 1;
};

}  // namespace story
}  // namespace tombstone
}  // namespace ts
