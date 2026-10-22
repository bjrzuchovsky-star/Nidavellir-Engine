#pragma once

// Saved panel layouts (Window > Layouts). Per user, beside settings.json:
// TombStoneConfig/layouts/<2d|3d>/<name>.ini, each one a snapshot of ImGui's
// window + docking state. imgui.ini itself is left to ImGui: nothing here
// writes it, and an upgrade never replaces the layout the user has now.
//
// Loading is queued and applied at the start of the next editor frame
// (apply_pending_layout(), before any panel is drawn), which is when ImGui
// can take a new docking tree.

#include <string>
#include <vector>

namespace ts::tombstone::editor::ux {

std::string layouts_dir(bool three_d);
// "My layout!" -> "My layout" (letters, digits, space, - and _; at most 48).
std::string clean_layout_name(const std::string& name);
std::vector<std::string> list_layouts(bool three_d);  // sorted names
bool layout_exists(bool three_d, const std::string& name);
// Snapshot the current panels into <name>.ini (replaces one of that name).
bool save_layout(bool three_d, const std::string& name, std::string* error = nullptr);
// Queue <name>.ini for the next frame.
bool queue_load_layout(bool three_d, const std::string& name, std::string* error = nullptr);
bool delete_layout(bool three_d, const std::string& name, std::string* error = nullptr);
// Start of frame: apply a queued layout. True if one was applied.
bool apply_pending_layout();
bool layout_pending();

}  // namespace ts::tombstone::editor::ux
