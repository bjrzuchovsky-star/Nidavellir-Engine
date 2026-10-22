#pragma once

// The project side of The Vault: what "Send to Vault", "Pull", "Update"
// and ts_admin --vault do to a claim. Headless (no ImGui); the Vault panel
// runs these on a worker thread.
//
// Push: the asset (or prefab) and everything it needs become library
// items: a model's sibling files (.bin / .mtl / .vvd / .vtx / .phy) ride in
// the same item, the textures it names (glTF images, OBJ material maps)
// become dependency items with their own GUIDs, a prefab depends on the
// sprites / models / sounds / prefabs it names. Each item carries its
// .meta (GUID + import settings), a thumbnail and a new version when the
// content changed.
//
// Pull: copies the files and the .meta with the same GUID to the same
// project-relative paths (or where that GUID already lives in this
// project), so scenes and prefabs that name them work in every claim.
// Recorded in <project>/.vault/lock.json (origin, version, pin, hashes).

#include "editor/assets/AssetDb.h"
#include "vault/VaultBackend.h"
#include "vault/VaultLock.h"

#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace vaultp {

struct PushOptions {
  std::string name;  // "" = file stem
  std::string author;
  std::string licence;
  std::string description;
  std::string note;  // version note
  std::vector<std::string> tags;
  std::vector<std::string> dims;  // "2d" / "3d" / "vr"
  std::string thumbnail_png;      // optional picture (absolute path); else made from the asset
};

struct PackFile {
  std::string rel;
  std::string role;  // main | file | meta
  std::string guid;
  std::string sha;
  std::uint64_t size = 0;
};

// One item to push (deps listed by their main file).
struct Package {
  std::string item;  // id (GUID)
  std::string kind;  // texture | model | audio | font | prefab
  std::string main;
  std::string guid;
  std::vector<PackFile> files;
  std::vector<std::string> dep_mains;
};

// What a push of `rel` would send, deps first (the last one is `rel`).
bool plan_push(const std::string& project_dir, assetdb::AssetDb& db, const std::string& rel,
               std::vector<Package>* out, std::string* error);

struct PushResult {
  std::string item;
  int version = 0;
  bool created = false;
  std::vector<std::string> pushed;  // "name vN" for every item, deps first
  std::uint64_t bytes = 0;
};

// db: an open, scanned database for the project (the Asset Browser's), or
// null to scan one here.
bool push_asset(const std::string& project_dir, assetdb::AssetDb* db, const std::string& rel,
                vault::Backend& backend, const PushOptions& opt, PushResult* out, std::string* error);

struct PullOptions {
  int version = 0;     // 0 = latest
  bool force = false;  // overwrite files changed in this project since the last pull
  bool pin = false;    // keep this version when updating everything
};

struct PullResult {
  std::string item;
  std::string main;  // project-relative main file
  std::string kind;
  int version = 0;
  std::vector<std::string> written;  // project-relative files written
  std::vector<std::string> items;    // "name vN" pulled (deps first)
};

bool pull_item(const std::string& project_dir, assetdb::AssetDb* db, vault::Backend& backend,
               const std::string& item, const PullOptions& opt, PullResult* out, std::string* error);

struct ItemStatus {
  std::string item, name, kind, main, origin, library;
  int version = 0;
  int latest = 0;
  bool pinned = false;
  bool update_available = false;
  bool modified = false;  // a file differs from what was pulled / pushed
  bool missing = false;   // the library no longer has it
};

// Lock entries for this backend, compared with the library.
bool project_status(const std::string& project_dir, vault::Backend& backend,
                    std::vector<ItemStatus>* out, std::string* error);
// Pulls the latest version of every pulled, unpinned item with an update.
bool update_all(const std::string& project_dir, assetdb::AssetDb* db, vault::Backend& backend,
                std::vector<std::string>* updated, std::string* error);
bool set_pinned(const std::string& project_dir, const std::string& item, bool pinned, std::string* error);

// Small PNG (<= 128 px) for a texture; false when it does not decode.
bool make_thumbnail(const std::string& image_abs, const std::string& out_png, std::string* error);
// Library thumbnail in a local cache folder (downloads once). "" when none.
std::string cached_thumbnail(vault::Backend& backend, const std::string& sha, const std::string& cache_dir);

// Kind name the library uses for a project path ("" when not shareable).
std::string vault_kind(const std::string& rel);

}  // namespace vaultp
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
