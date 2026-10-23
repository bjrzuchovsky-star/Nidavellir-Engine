#pragma once

// 0.2.0 plugin split: physics and player movement are optional Trading Post
// plugins instead of being built into every game.
//
//   ts.physics.jolt     Jolt Physics (3D / VR)       physics3d::World backend
//   ts.physics.box2d    Box2D Physics (2D)           physics2d::World backend
//   ts.move.rider3d     Rider 3D (first / third)     world3d::RiderController3D
//   ts.move.topdown     Top-down Rider (2D)          runtime::RiderController2D
//   ts.move.platformer  Platformer Rider (2D)        runtime::RiderController2D
//
// This header also holds what keeps old claims and new templates working:
// the needs scan (which of these a claim's scenes use), the one-time
// migration that adds them to a claim's plugin list (logged, with a backup
// of project.json), and an RAII hold for hosts that ride a world without a
// PluginManager (ts_game's 3D / VR path, the smoke harness).

#include "plugin/Catalog.h"
#include "plugin/Plugin.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace plugins {

inline constexpr const char* kJoltId = "ts.physics.jolt";
inline constexpr const char* kBox2DId = "ts.physics.box2d";
inline constexpr const char* kRider3DId = "ts.move.rider3d";
inline constexpr const char* kTopDownId = "ts.move.topdown";
inline constexpr const char* kPlatformerId = "ts.move.platformer";

// The five ids above, in catalog order.
const std::vector<std::string>& split_plugin_ids();
bool is_split_plugin(const std::string& id);

// Adds the five plugins to a catalog (register_builtin_plugins does).
void register_split_plugins(plugin::Catalog& catalog, std::vector<std::string>* errors);

// Offers the 2D implementations (Box2D, top-down, platformer) to the core
// registries; register_builtin_plugins does it. 3D ones: offer_3d_providers
// (plugins/rider3d/Rider3D.h) in hosts that run World3D.
void offer_builtin_providers();

// Switch provider `id` on / off for this process (ref-counted). The split
// plugins do this in on_load / on_unload; ProviderHold for other hosts.
void acquire_provider(const std::string& id);
void release_provider(const std::string& id);
bool provider_active(const std::string& id);

class ProviderHold {
 public:
  ProviderHold() = default;
  explicit ProviderHold(const std::vector<std::string>& ids) { hold(ids); }
  ~ProviderHold() { clear(); }
  ProviderHold(const ProviderHold&) = delete;
  ProviderHold& operator=(const ProviderHold&) = delete;
  // Holds the split plugin ids among `ids` (others are ignored).
  void hold(const std::vector<std::string>& ids);
  void clear();
  const std::vector<std::string>& held() const { return held_; }

 private:
  std::vector<std::string> held_;
};

// ---- Needs scan -------------------------------------------------------------

// Split plugins a claim's content uses, in split_plugin_ids() order. Scans
// scene.json, *.scene.json / *.scene3d.json / *.prefab.json (dot folders,
// export / build / backup folders skipped) and Shipyard sky files. `why`
// (optional) gets one line per need ("ts.move.rider3d: Player on 'Rider'
// in scene.json").
//   3D: player -> rider3d + jolt; vr_rig -> jolt + rider3d (desktop
//       fallback); rigid_body / grabbable / joint -> jolt.
//   2D: player -> topdown or platformer by mode; body / joint -> box2d;
//       Shipyard "hull_physics": "box2d" -> box2d.
std::vector<std::string> required_plugins(const std::string& claim_dir,
                                          plugin::ProjectType type,
                                          std::vector<std::string>* why = nullptr);
// Same, reading the type from project.json.
std::vector<std::string> required_plugins(const std::string& claim_dir,
                                          std::vector<std::string>* why = nullptr);

// "ts.physics.jolt, ts.move.rider3d" -- the active providers -- or
// "none" (hosts print it so a ride's log says what it rode with).
std::string provider_summary();

// ---- Migration --------------------------------------------------------------

// Marker of a claim that is past the split (written by the migration, by
// new claims from templates, and baked into exports as "plugin_split").
inline constexpr const char* kMarkerRel = ".tombstone/plugin_split.json";
inline constexpr const char* kMigrationLogRel = ".tombstone/migrations.log";
inline constexpr const char* kBackupDirRel = ".tombstone/backup";

// Marker file present, or project.json says "plugin_split".
bool claim_is_split(const std::string& claim_dir);

struct MigrationReport {
  bool ran = false;       // the claim was not split yet
  bool changed = false;   // project.json got new plugins
  std::vector<std::string> added;    // split plugins added
  std::vector<std::string> why;      // needs-scan reasons
  std::vector<std::string> enabled;  // the plugin list afterwards
  std::string backup;     // project.json copy (when changed)
  std::string error;      // empty = fine
  std::vector<std::string> log;      // lines for the editor's Telegraph
};

// One-time migration of a pre-0.2.0 claim (EditorPlugins::open runs it):
// add the needed split plugins to project.json "plugins" (a claim without
// a list starts from `defaults`), back project.json up first, write the
// marker and append to .tombstone/migrations.log. A split claim is left
// alone (ran = false), so plugins the user later switches off stay off.
MigrationReport migrate_claim(const std::string& claim_dir,
                              const std::vector<std::string>& defaults);

// A claim just staked from a template (or blank): add what its scenes
// need to project.json "plugins" (a claim without a list starts from
// `defaults`), then mark it split. No backup -- nothing to keep. Returns
// the split plugins added.
std::vector<std::string> settle_new_claim(const std::string& claim_dir,
                                          const std::vector<std::string>& defaults,
                                          std::string* error = nullptr);

// Mark a fresh claim as split (templates; nothing else changes).
bool mark_claim_split(const std::string& claim_dir, const std::vector<std::string>& added,
                      std::string* error = nullptr);

// The list a runtime host should load: project.json's (else `defaults`),
// plus -- for a claim not migrated yet -- the needed split plugins, in
// memory only (*added says which).
std::vector<std::string> effective_plugins(const std::string& claim_dir,
                                           const std::vector<std::string>& listed, bool is_listed,
                                           const std::vector<std::string>& defaults,
                                           std::vector<std::string>* added = nullptr);

}  // namespace plugins
}  // namespace tombstone
}  // namespace ts
