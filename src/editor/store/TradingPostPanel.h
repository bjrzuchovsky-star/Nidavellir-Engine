#pragma once

// The Trading Post window (ImGui, Dusk theme): browse / search the plugin
// catalog by category, read details (version, deps, project types, source,
// hash), install / update / uninstall into the engine plugin cache, and
// hitch a plugin to a claim. Opened from the Project Manager (for the
// highlighted claim) and from the Outfitter's Wagon (for the open claim).

#include "editor/ux/FriendlyError.h"
#include "editor/ProjectInfo.h"
#include "store/TradingPost.h"

#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

class EditorPlugins;

struct TradingPostUi {
  bool open = false;
  bool loaded = false;  // refreshed at least once
  std::unique_ptr<store::TradingPost> post;
  char search[64] = {};
  std::string category;  // "" = all
  std::string selected;  // entry id
  std::string status;
  std::string error;
  bool show_sources = false;
  char new_folder[260] = {};
  char new_url[512] = {};
  char new_key[160] = {};
};

// Who "enable" applies to: the open editor's plugins (live, with restart-
// of-play) or just a project on disk (Project Manager). Both may be null.
struct TradingPostTarget {
  ProjectInfo* project = nullptr;
  EditorPlugins* plugins = nullptr;
  std::string project_dir;  // for the plugin search dirs ("" = none)
};

struct TradingPostResult {
  bool installed_changed = false;  // the cache changed: rescan the wagon
  bool enabled_changed = false;
};

// Draws the window when ui.open (a normal ImGui window; call once a frame).
TradingPostResult draw_trading_post_window(TradingPostUi& ui, const TradingPostTarget& target);

// (Re)builds the catalog: built-ins + editor built-ins + search dirs for
// `project_dir`, cache, sources, coming soon.
void trading_post_refresh(TradingPostUi& ui, const std::string& project_dir);

// Hover text for a Trading Post row: what it is, where it works, where it
// comes from, what state it's in and what to do next.
std::string trading_post_entry_tip(const store::StoreEntry& e, bool hitched);
// The panel's error card (network, signature and install failures).
ux::FriendlyError friendly_trading_post_error(const std::string& raw);

// Project Manager path (no editor open): switch `id` on / off in
// project.json. On adds its required deps (from the Trading Post's
// catalog); off removes what requires it. Old projects start from their
// kind's defaults.
bool set_plugin_enabled_in_project(ProjectInfo& project, const std::string& id, bool on,
                                   const store::TradingPost& post, std::string* error,
                                   std::vector<std::string>* also = nullptr);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
