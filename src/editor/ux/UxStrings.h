#pragma once

// Every Field Guide string in one place: tooltips for toolbar buttons and
// Inspector fields (keyed by widget label, optionally "Section/Label"),
// menu tips, empty-panel guidance and the walkthrough steps. Plain English
// first; the frontier names stay where the panels already use them.

#include <string>
#include <vector>

namespace ts::tombstone::editor::ux {

// Dictionary text for a key, nullptr when there is none.
const char* dictionary_text(const std::string& key);
// Bare labels the dictionary knows (the part after "Section/"), for
// matching multi-part widgets by ID.
const std::vector<std::string>& dictionary_labels();
std::size_t dictionary_size();

struct EmptyText {
  const char* title;
  const char* body;
};
// "hierarchy2d" "hierarchy3d" "assets" "scripts" "vault" "telegraph"
// "inspector2d" "inspector3d" "prefabs". Unknown ids give a generic line.
EmptyText empty_text(const std::string& id);

struct WalkStep {
  const char* window;  // ImGui window name to point at ("" = centre)
  const char* panel;   // EditorUx panel label to show first ("" = none)
  const char* title;
  const char* body;
};
const std::vector<WalkStep>& walkthrough_steps(bool three_d);

// Settings screen sections, in order (title, what lives there).
struct SettingsSection {
  const char* title;
  const char* blurb;
};
const std::vector<SettingsSection>& settings_sections();

}  // namespace ts::tombstone::editor::ux
