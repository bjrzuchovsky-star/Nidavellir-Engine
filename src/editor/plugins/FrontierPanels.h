#pragma once

// Editor panels for the Frontier Survival plugin (ts.frontier): the
// Saddlebags (inventory, hotbar, crafting during Play) and the Prospector
// (seed, reroll, a world preview and "burn the claim"). Installed through
// SurvivalPlugin::set_editor_hook by register_editor_builtin_plugins.

namespace ts {
namespace tombstone {
namespace editor {

void install_frontier_panels();

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
