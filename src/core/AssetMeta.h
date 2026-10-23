#pragma once

// Where an asset's metadata (.meta: GUID + import settings, see
// editor/assets/AssetDb.h) lives. Since the metadata folder (claim layout
// 2) every .meta sits in one hidden folder of the claim that mirrors the
// asset paths, so asset folders on disk hold only real assets:
//
//   <claim>/assets/Models/crate/crate.gltf
//   <claim>/.tombstone/meta/assets/Models/crate/crate.gltf.meta
//
// Older claims kept a sidecar "<file>.meta" next to each asset; the editor
// moves those into the folder when the claim opens (AssetDb::migrate).
// Shipped games (game.tspak) still carry a model's .meta as a sidecar,
// which is what the runtime model importer falls back to.
//
// The Vault, exports and lock files name a .meta by its logical path
// "<asset rel>.meta" (the sidecar name); resolve() maps that to the file.

#include <string>

namespace ts {
namespace tombstone {
namespace core {
namespace asset_meta {

inline constexpr const char* kMetaExt = ".meta";
inline constexpr const char* kMetaDir = ".tombstone/meta";           // the mirror
inline constexpr const char* kLayoutFile = ".tombstone/meta.json";   // {"meta_layout": 2}
inline constexpr const char* kLogFile = ".tombstone/meta.log";       // migration / relink log
inline constexpr const char* kBackupDir = ".tombstone/meta_backup";  // sidecars before migration
inline constexpr const char* kOrphanDir = ".tombstone/meta_orphans"; // cleaned-up orphans
inline constexpr int kLayoutVersion = 2;

// "<claim>/.tombstone/meta"
std::string store_dir(const std::string& claim_dir);
// "<claim>/.tombstone/meta/<asset_rel>.meta" (whether or not it exists).
std::string meta_path(const std::string& claim_dir, const std::string& asset_rel);
// Legacy "<claim>/<asset_rel>.meta".
std::string sidecar_path(const std::string& claim_dir, const std::string& asset_rel);
// True for "<something>.meta".
bool is_meta_rel(const std::string& rel);
// "a/b.png.meta" -> "a/b.png" (unchanged when not a .meta).
std::string asset_of(const std::string& meta_rel);
// A logical "<asset rel>.meta" (or a ".tombstone/meta/..." path) -> the
// file holding it: the metadata folder first, then a legacy sidecar. ""
// when neither exists.
std::string resolve(const std::string& claim_dir, const std::string& logical_meta_rel);
// Same for an asset ("assets/a.png"), returning its .meta file or "".
std::string find_for_asset(const std::string& claim_dir, const std::string& asset_rel);
// For an absolute asset path when the claim is not known (runtime model
// import): walks up the parent folders for a ".tombstone/meta" mirror
// holding it, else the sidecar "<path>.meta". "" when none exists.
std::string find_for_file(const std::string& abs_asset_path);

}  // namespace asset_meta
}  // namespace core
}  // namespace tombstone
}  // namespace ts
