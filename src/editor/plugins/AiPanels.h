#pragma once

// ts.ai editor panels: the Behaviour Trees panel (edit *.bt.json with a
// palette from bt_node_types, lint, save; live node states, the running
// branch and the blackboard of the agents riding in 2D or 3D Play; the AI
// debug overlay toggle). Installed through AiPlugin::set_editor_hook by
// register_editor_builtin_plugins.

#include "ai/BehaviorTree.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace plugins {
class AiPlugin;
}
namespace editor {

void install_ai_panels();
// Panel state (one per editor); the smoke draws it in a headless ImGui frame.
struct BtPanelState {
  std::vector<std::string> files;  // project-relative *.bt.json
  std::string path;                // open file
  ai::BtTree tree;
  bool dirty = false;
  int selected = 0;
  std::string status;
  std::string new_name = "guard";
  int palette = 0;
  std::uint64_t watch = 0;  // live agent
  double scanned = -1.0;
  std::vector<std::string> lint;
};
void draw_bt_panel(plugins::AiPlugin& p, BtPanelState& s);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
