#pragma once

// Command palette (Ctrl+Shift+P): fuzzy search over every action the
// registry knows (menu items, panels, create items, Settings sections,
// recent claims and assets). Shows shortcuts and tips, greys out what can't
// run right now, Enter runs the highlighted entry.

#include "editor/ux/Actions.h"

#include <string>
#include <string_view>
#include <vector>

namespace ts::tombstone::editor::ux {

// Case-insensitive fuzzy score: every query character in order. Higher is
// better (whole-word and prefix hits rank first); -1 = no match.
int fuzzy_score(std::string_view query, std::string_view text);

// Indexes into `actions`, best first (disabled entries after enabled ones).
std::vector<std::size_t> palette_rank(std::string_view query, const std::vector<Action>& actions);

// What the palette searches for one action: "Edit > Duplicate  Ctrl+D ...".
std::string palette_haystack(const Action& a);

class CommandPalette {
 public:
  void open();
  void close() { open_ = false; }
  bool is_open() const { return open_; }

  // Draws when open; returns the id the user chose ("" = none this frame).
  std::string draw(const std::vector<Action>& actions);

  // Smoke: set the query text directly.
  void set_query(const std::string& q);
  const std::string& query() const { return query_; }

 private:
  bool open_ = false;
  bool want_open_ = false;
  bool focus_input_ = false;
  bool reload_input_ = false;
  std::string query_;
  char buf_[256] = {};
  int highlight_ = 0;
};

}  // namespace ts::tombstone::editor::ux
