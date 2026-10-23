#pragma once

// Field Guide preferences: tooltips on/off + delay, which walkthroughs have
// been seen, and the command palette's recent list. Per user: they ride in
// TombStoneConfig/settings.json with the rest of Settings (never per claim).
// The editor reads/writes ux::prefs(); AppFlow folds them back into its own
// Settings copy before it saves so neither side clobbers the other.

#include "editor/settings/Settings.h"

#include <string>
#include <vector>

namespace ts::tombstone::editor::ux {

struct RecentItem {
  std::string kind;   // "scene" | "asset" | "claim"
  std::string path;   // claim-relative for assets, folder for claims
  std::string label;  // shown in the palette
};

struct Prefs {
  bool tooltips = true;
  float tooltip_delay = 0.5f;  // seconds, 0..3
  bool walkthrough_2d_done = false;
  bool walkthrough_3d_done = false;
  std::vector<RecentItem> recent;  // newest first, at most kMaxRecent
};

inline constexpr std::size_t kMaxRecent = 12;

Prefs& prefs();

void import_from(const Settings& s);
void export_to(Settings* s);
// Load settings.json, fold the prefs in, save. False + error on I/O trouble.
bool persist(std::string* error = nullptr);

// Push to the front of the recent list (dedupes on kind + path) and persist.
void add_recent(const std::string& kind, const std::string& path, const std::string& label);

std::string format_delay(float seconds);  // "0.50"

}  // namespace ts::tombstone::editor::ux
