#pragma once

#include "editor/ProjectInfo.h"

namespace ts {
namespace tombstone {
namespace editor {

// Realm / File > Project settings. The menu item only asks; the modal is
// drawn later, off the menu's ID stack (a menu closes the moment it is clicked).
void draw_project_settings_menu(ProjectInfo& project);
void draw_project_settings_popup(ProjectInfo& project);

// 0 engine default, 1 OpenGL, 2 Vulkan, 3 Direct3D 12. Writes project.json.
void save_project_graphics_choice(ProjectInfo& project, int choice);

// The project whose graphics API just changed, if the menu saved one.
bool take_graphics_relaunch(ProjectInfo* project);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
