#include "editor/plugins/PluginPanelHost.h"

#include <imgui.h>

#include "editor/plugins/EditorPlugins.h"

namespace ts {
namespace tombstone {
namespace editor {

int draw_plugin_panel_menu_items(EditorPlugins& plugins, std::map<std::string, bool>& open) {
  if (!plugins.is_open()) return 0;
  int n = 0;
  for (const auto& p : plugins.manager().editor_panels()) {
    bool* flag = p.item.open ? p.item.open : &open[p.item.id];
    ImGui::MenuItem(p.item.menu_label.c_str(), nullptr, flag);
    ++n;
  }
  return n;
}

void draw_open_plugin_panels(EditorPlugins& plugins, std::map<std::string, bool>& open) {
  if (!plugins.is_open()) return;
  // A draw callback may change plugin state, never unload (reloads wait
  // for idle and happen outside the panel loop).
  for (const auto& p : plugins.manager().editor_panels()) {
    bool* flag = p.item.open ? p.item.open : &open[p.item.id];
    if (!*flag) continue;
    if (p.item.before_begin) p.item.before_begin();
    const std::string title = p.item.title + "###plugin_" + p.item.id;
    // The posse's Network panel keeps its old window id for saved layouts.
    const char* window = p.item.title == "Network" ? "Network" : title.c_str();
    if (ImGui::Begin(window, flag)) p.item.draw();
    ImGui::End();
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
