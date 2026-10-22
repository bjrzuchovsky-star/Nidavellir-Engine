#pragma once
// Field Guide empty states: a panel with nothing in it says what goes here
// and offers the next step as buttons. Text lives in UxStrings
// (empty_text(id)); --smoke-ux checks every panel draws one when empty.

#include <functional>
#include <initializer_list>
#include <set>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {
namespace ux {

struct EmptyButton {
  const char* label;
  std::function<void()> run;
  const char* tip = nullptr;
  bool enabled = true;
};

// Draws the title, guidance and buttons (first button is the primary one).
// Returns true when a button ran. `body_override` replaces the stock text
// (search misses: "Nothing matches 'crate'").
bool EmptyState(const char* id, std::initializer_list<EmptyButton> buttons = {},
                const char* body_override = nullptr);

// Ids drawn since the last reset (smoke).
const std::set<std::string>& empty_states_seen();
void reset_empty_states_seen();

}  // namespace ux
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
