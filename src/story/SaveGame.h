#pragma once

// Strongbox (ts.saves) core: named save slots and a rotating autosave on
// disk, each a JSON file of sections that providers fill and read back.
//
//   <project>/saves/slots/<slot>.save.json
//   {"format": 1, "slot": "1", "label": "Before the bell", "time": "2026-...Z",
//    "playtime": 412.5, "scene": "Ghost Town 3D", "auto": false,
//    "sections": {"vars": {...}, "quests": {...}, "dialogue": {...},
//                 "data": {...}, "player": {...}}}
//
// Slot names: letters, digits, '-', '_' (max 32). Autosaves rotate through
// "auto-1".."auto-N" (newest wins, oldest overwritten). Writes go to a temp
// file first and are renamed into place, so a crash mid-save keeps the old
// slot.

#include "scene/JsonTree.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace story {

struct SlotInfo {
  std::string slot;
  std::string label;
  std::string time;  // ISO 8601 UTC
  double playtime = 0.0;
  std::string scene;
  bool autosave = false;
  long long stamp = 0;  // sort key (seconds since epoch)
};

bool valid_slot_name(const std::string& s);

class SaveStore {
 public:
  static constexpr int kFormat = 1;
  explicit SaveStore(std::string project_dir = {});
  void set_project_dir(std::string dir) { dir_ = std::move(dir); }
  std::string slots_dir() const;

  struct Provider {
    std::string section;
    std::function<json::Value()> save;
    std::function<void(const json::Value&)> load;
  };
  void provide(Provider p);  // replaces a provider of the same section
  void clear_providers() { providers_.clear(); }
  std::vector<std::string> sections() const;

  // What a save records about the game around it.
  std::string scene;
  double playtime = 0.0;
  int autosave_slots = 3;

  bool save(const std::string& slot, const std::string& label, std::string* error = nullptr);
  // The next autosave slot ("auto-1".."auto-N", oldest overwritten).
  bool autosave(std::string* slot_out = nullptr, std::string* error = nullptr);
  bool load(const std::string& slot, std::string* error = nullptr);
  bool remove(const std::string& slot);
  bool exists(const std::string& slot) const;
  // Newest first.
  std::vector<SlotInfo> list() const;
  bool info(const std::string& slot, SlotInfo* out) const;
  std::string latest() const;  // newest slot, "" when none

  std::uint64_t saves_written() const { return written_; }
  std::uint64_t loads_done() const { return loaded_; }

 private:
  std::string path(const std::string& slot) const;
  bool write(const std::string& slot, const std::string& label, bool autosave, std::string* error);

  std::string dir_;
  std::vector<Provider> providers_;
  std::uint64_t written_ = 0, loaded_ = 0;
};

}  // namespace story
}  // namespace tombstone
}  // namespace ts
