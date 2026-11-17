#pragma once

#include "editor/ProjectInfo.h"

namespace ts {
namespace tombstone {
namespace editor {

// Realm / File > Project settings. Saving a different API queues a relaunch.
void draw_project_settings_menu(ProjectInfo& project);

// The project whose graphics API just changed, if the menu saved one.
bool take_graphics_relaunch(ProjectInfo* project);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
