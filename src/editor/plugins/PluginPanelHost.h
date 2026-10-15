#pragma once
// Shared by the 2D and 3D editor shells: a plugin's editor panels get a
// View menu entry and a window, in edit mode and during Play alike.
#include <map>
#include <string>

namespace ts {
namespace tombstone {
namespace editor {

class EditorPlugins;

// One MenuItem per plugin panel (call inside BeginMenu("View")).
// Returns the number of entries drawn.
int draw_plugin_panel_menu_items(EditorPlugins& plugins, std::map<std::string, bool>& open);
// Begin/draw/End every open plugin panel.
void draw_open_plugin_panels(EditorPlugins& plugins, std::map<std::string, bool>& open);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
