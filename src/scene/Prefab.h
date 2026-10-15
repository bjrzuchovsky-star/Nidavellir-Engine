#pragma once

// Prefabs (.prefab.json assets) shared by the 2D and 3D editors. The
// algorithms work on entities as JSON objects (the same objects scene.json
// writes), so one implementation serves Entity2D and Entity3D; each editor
// converts with its scene codec (scene_json::entity_to_json /
// scene3d::entity_to_json). No UI.
//
// Asset:   {"prefab": 1, "dimension": "2d"|"3d", "name": "...",
//           "entities": [ {"id": 1, ...}, ... ]}
//          Ids are local (1..n), parents point at local ids, positions are
//          offsets from the first root (which sits at 0,0[,0]).
// Instance: every placed entity carries a "prefab" link
//          {"asset": "prefabs/crate.prefab.json", "instance": 7,
//           "source": 2, "overrides": ["name", "material"]}.
// Sync rule: a field (top-level entity key) follows the asset unless it is
// listed in overrides. Positions are per instance: the entity linked to the
// asset's first root is the instance origin; the others sit at origin +
// asset offset unless "position" is overridden. id / parent / prefab /
// locked / hidden never sync.

#include "scene/JsonTree.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace prefab {

enum class Dim { D2, D3 };

inline constexpr const char* kExtension = ".prefab.json";
inline constexpr const char* kFolder = "prefabs";
// Override name used for the position fields ("x"/"y" in 2D, "position"
// in 3D).
inline constexpr const char* kPositionKey = "position";

struct Link {
  std::string asset;  // project-relative, '/' separators
  std::uint64_t instance = 0;
  std::uint64_t source = 0;  // local id inside the asset
  std::vector<std::string> overrides;  // sorted, unique
  bool overridden(const std::string& key) const;
  bool operator==(const Link& o) const = default;
};

json::Value link_to_json(const Link& link);
bool link_from_json(const json::Value& v, Link* out);

struct Asset {
  Dim dim = Dim::D2;
  std::string name;
  std::vector<json::Value> entities;  // local ids, positions relative
};

bool parse_asset(const std::string& text, Asset* out, std::string* error);
std::string write_asset(const Asset& asset);
bool load_asset(const std::string& path, Asset* out, std::string* error);
bool save_asset(const std::string& path, const Asset& asset, std::string* error);

// "Crate Stack" -> "prefabs/crate_stack.prefab.json".
std::string default_asset_rel(const std::string& name);
// Display name from a path ("prefabs/crate.prefab.json" -> "crate").
std::string name_from_rel(const std::string& rel);
// Every *.prefab.json under the project (relative paths, sorted).
std::vector<std::string> scan_assets(const std::string& project_dir);

// ---- Entity-JSON helpers ----------------------------------------------------
std::uint64_t json_id(const json::Value& e);
std::uint64_t json_parent(const json::Value& e);
bool json_link(const json::Value& e, Link* out);  // false when not an instance
// Copy of `o` with `key` set (added at the end) / removed.
json::Value with_key(const json::Value& o, const std::string& key, json::Value v);
json::Value without_key(const json::Value& o, const std::string& key);
// Read / write the entity position (2 or 3 numbers).
void get_pos(Dim dim, const json::Value& e, double out[3]);
json::Value set_pos(Dim dim, const json::Value& e, const double p[3]);
// Same JSON (numbers compared exactly through their written text).
bool json_equal(const json::Value& a, const json::Value& b);

// Selection (entities with their subtrees, scene order) -> asset. Links on
// the inputs are dropped (nested prefabs are unpacked).
// local_ids (optional) receives scene id -> asset local id.
Asset make_asset(Dim dim, const std::string& name,
                 const std::vector<json::Value>& entities,
                 std::map<std::uint64_t, std::uint64_t>* local_ids = nullptr);

// New instance entities: ids from *next_id up (advanced), first root at
// `pos`, linked to asset_rel / instance_id. Root parents are `parent`.
std::vector<json::Value> instantiate(const Asset& asset, const std::string& asset_rel,
                                     const double pos[3], std::uint64_t instance_id,
                                     std::uint64_t* next_id, std::uint64_t parent = 0);

// Largest instance id used in the scene (new instances take max + 1).
std::uint64_t max_instance(const std::vector<json::Value>& scene);

// Returns the asset for a project-relative path (nullptr when it does not
// load). Callers usually cache.
using AssetLookup = std::function<const Asset*(const std::string& rel)>;

// Recompute every instance entity's override list from what differs from
// its asset. Duplicated instance entities (copy / paste / Ctrl+D) are split
// off into fresh instances. True when anything changed.
bool refresh_overrides(Dim dim, std::vector<json::Value>& scene,
                       const AssetLookup& lookup);

// Bring every instance of asset_rel in line with `asset`: non-overridden
// fields copied, entities added to / removed from the asset added /
// removed. *next_id hands out new entity ids. Returns how many entities
// changed.
std::size_t sync_instances(Dim dim, std::vector<json::Value>& scene,
                           const std::string& asset_rel, const Asset& asset,
                           std::uint64_t* next_id);

// Push an instance entity's override(s) into the asset (key "" = all of
// that entity's overrides; entity_id 0 = every entity of the instance the
// entity belongs to). The overrides are cleared on the instance. False when
// nothing applied.
bool apply_overrides(Dim dim, std::vector<json::Value>& scene,
                     std::uint64_t entity_id, const std::string& key,
                     bool whole_instance, Asset* asset);
// Drop override(s) (key "" = all; whole_instance = every entity of the
// instance) and take the asset's values again.
bool revert_overrides(Dim dim, std::vector<json::Value>& scene,
                      std::uint64_t entity_id, const std::string& key,
                      bool whole_instance, const Asset& asset,
                      std::uint64_t* next_id);

// Remove the link from every entity of the instance (plain entities).
std::size_t unpack_instance(std::vector<json::Value>& scene, std::uint64_t entity_id);

}  // namespace prefab
}  // namespace tombstone
}  // namespace ts
