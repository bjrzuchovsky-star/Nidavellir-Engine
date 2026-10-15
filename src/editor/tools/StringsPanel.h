#pragma once

// Strings (Localisation) panel, Dusk theme: the project's string tables as
// a key x language grid. Missing translations are highlighted, as are keys
// the scripts / UI use (tr("...") / "text_key") that no table defines.
// Edit, add keys and languages, preview a language, save back to
// strings/*.json / strings.csv. Shared by the 2D and 3D editors.

#include "core/Localization.h"

#include <set>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

class StringsPanel {
 public:
  static StringsPanel& get();
  void open(const std::string& project_dir, const std::string& project_name);
  bool is_open() const { return open_; }
  void draw();
  void pin(float x, float y, float w, float h) {
    pinned_ = true;
    pin_[0] = x; pin_[1] = y; pin_[2] = w; pin_[3] = h;
  }
  // Keys used by scripts / UI documents of a project (tr("k"), Tr("k"),
  // "text_key": "k").
  static std::set<std::string> scan_used_keys(const std::string& project_dir);
  // Missing translations per language plus used-but-undefined keys.
  int problem_count() const;
  void select(const std::string& key) { selected_ = key; }

  // ImGui's font gets the OS / project fallback fonts merged in (CJK,
  // Cyrillic, ...) so non-Latin strings show. Call between frames.
  static void apply_pending_fonts();
  static void request_fonts(const std::string& project_dir);

 private:
  void reload();
  void save();

  bool open_ = false;
  bool pinned_ = false;
  float pin_[4] = {};
  std::string project_dir_, project_name_;
  loc::StringTables tables_;
  std::set<std::string> used_;
  std::string selected_;
  std::string status_;
  char filter_[96] = {};
  char new_key_[96] = {};
  char new_lang_[16] = {};
  bool missing_only_ = false;
  bool dirty_ = false;
  std::vector<std::string> edit_buf_;  // per language for the selected key
  std::string edit_key_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
