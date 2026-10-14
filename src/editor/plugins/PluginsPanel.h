#pragma once

// The Outfitter's Wagon: the Plugins panel (ImGui, Dusk theme). Shared by
// the 2D editor and the 3D / VR frontier screen.

#include "editor/plugins/EditorPlugins.h"

#include <string>

namespace ts {
namespace tombstone {
namespace editor {

struct PluginsPanelState {
  char filter[64] = {};
};

struct PluginsPanelResult {
  bool changed = false;          // the enabled list changed this frame
  bool rescan_requested = false; // "Rescan Wagon"
};

// riding: a ride is going, so changes wait for it to end.
PluginsPanelResult draw_plugins_panel(EditorPlugins& plugins,
                                      PluginsPanelState& state, bool riding);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
