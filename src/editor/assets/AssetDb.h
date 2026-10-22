#pragma once

// Project asset database: every importable file (images, 3D models of any
// supported format - glTF, Source .mdl, FBX / OBJ / DAE... see
// render3d/ModelImport.h - sounds, fonts) gets a .meta holding a stable
// GUID, its import settings and what the importer found. The .meta files
// live in the claim's hidden metadata folder, mirroring the asset paths
// (core/AssetMeta.h):
//
//   assets/Textures/crate.png  ->  .tombstone/meta/assets/Textures/crate.png.meta
//
// so asset folders hold only real assets. The GUID is the asset's
// identity: when a file is renamed or moved in the Asset Browser its .meta
// moves with it; when it is moved outside the editor (Explorer, git mv)
// the next scan finds the left-behind .meta by content hash (or, for an
// edited file, by a unique file name + type) and relinks it, logging the
// relink to .tombstone/meta.log. Either way every reference to the old
// path in the project's scenes, prefabs and other JSON / Lua files is
// rewritten, so nothing breaks. A .meta whose file is gone for good is an
// orphan: reported, and moved to .tombstone/meta_orphans/ (or deleted) by
// clean_orphans() once the user confirms. No UI (AssetBrowser draws it).
//
// Claims from before the folder kept a sidecar "<file>.meta" next to each
// asset: migrate() (run by open()) moves them into the folder with their
// GUIDs untouched, backs them up to .tombstone/meta_backup/<time>/, logs
// to .tombstone/meta.log and stamps .tombstone/meta.json
// {"meta_layout": 2}. It is idempotent, and a sidecar that shows up later
// (a file copied in from an old claim with its .meta) is adopted the same
// way by scan().
//
// .meta format (JSON):
//   {"meta": 1, "guid": "9f2c...32 hex", "kind": "texture",
//    "path": "assets/props/crate.png", "hash": "fnv1a64 hex",
//    "settings": {...per kind...}, "info": {...importer output...}}

#include "scene/JsonTree.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace assetdb {

enum class Kind { Texture, Model, Audio, Font, Unknown };

inline constexpr const char* kMetaExt = ".meta";
// Model importer revision stored in .meta info ("importer_version"); a
// lower one is reimported on the next scan (2 = bounds fix, 3 = Source
// materials through the content search folders).
inline constexpr int kModelImporterVersion = 3;

Kind kind_for(const std::string& path);   // by extension
const char* kind_name(Kind k);            // "texture" / "model" / ...
Kind kind_from_name(const std::string& name);
// Import settings a fresh .meta gets (models: per format, from the path).
json::Value default_settings(Kind k, const std::string& path = {});

struct Meta {
  std::string guid;
  Kind kind = Kind::Unknown;
  std::string path;   // project-relative, '/' separators (last known)
  std::string hash;   // content hash (16 hex)
  json::Value settings = json::Value::object();
  json::Value info = json::Value::object();
};

std::string new_guid();
std::string file_hash(const std::string& abs_path);  // "" when unreadable
// file_hash, folding in a model's sibling files (.vvd / .vtx / .phy, .mtl).
std::string asset_hash(const std::string& abs_path);
// Newest mtime / total size of the file and those siblings.
void asset_stat(const std::string& abs_path, std::filesystem::file_time_type* mtime,
                std::uintmax_t* size);
bool read_meta(const std::string& meta_abs, Meta* out, std::string* error = nullptr);
bool write_meta(const std::string& meta_abs, const Meta& meta, std::string* error = nullptr);

// Importer: validates the file and fills `info` (dimensions, triangles,
// duration, family name...). False + error when the file does not import.
using Importer = std::function<bool(const std::string& abs_path, Kind kind,
                                    const json::Value& settings, json::Value* info,
                                    std::string* error)>;

struct Ref {
  std::string file;  // project-relative file that mentions the asset
  int count = 0;
};

class AssetDb {
 public:
  struct Entry {
    Meta meta;
    std::string rel;
    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = 0;
    bool import_ok = true;
    std::string import_error;
  };
  struct ScanReport {
    std::vector<std::string> created;    // new assets (fresh GUID)
    std::vector<std::string> changed;    // content changed -> reimported
    std::vector<std::string> removed;    // known before, gone now
    std::vector<std::pair<std::string, std::string>> moved;  // old -> new
    std::vector<std::string> duplicated; // copies given a new GUID
    // Moves found through a left-behind .meta (file moved outside the
    // editor): old -> new, with how ("hash" or "name"). Also in `moved`.
    struct Relink {
      std::string from, to, how;
    };
    std::vector<Relink> relinked;
    std::vector<std::string> orphan_metas;  // assets whose .meta has no file (asset rel paths)
    std::vector<std::string> adopted;       // legacy sidecars moved into the metadata folder
    bool any() const {
      return !created.empty() || !changed.empty() || !removed.empty() || !moved.empty() ||
             !duplicated.empty();
    }
  };

  struct MigrationReport {
    int moved = 0;        // sidecars moved into the metadata folder
    int duplicates = 0;   // sidecars the folder already held (same GUID): removed
    int conflicts = 0;    // folder held another GUID: kept the folder's, sidecar backed up
    std::string backup_dir;  // project-relative, "" when nothing was backed up
    std::vector<std::string> assets;  // asset rel paths whose sidecar was handled
    bool any() const { return moved + duplicates + conflicts > 0; }
  };

  void set_importer(Importer importer) { importer_ = std::move(importer); }
  // Opens the claim and runs migrate() (legacy sidecars -> metadata folder).
  void open(const std::string& project_dir);
  // Moves sidecar .meta files next to assets into .tombstone/meta (GUIDs
  // kept), with a backup and a log. Safe to call again (no-op).
  MigrationReport migrate();
  const MigrationReport& last_migration() const { return migration_; }
  // The .meta file of an asset (in the metadata folder, existing or not).
  std::string meta_abs(const std::string& rel) const;
  // Orphans from scan(): move them to .tombstone/meta_orphans/ (keep =
  // true) or delete them. Returns how many were handled.
  int clean_orphans(const std::vector<std::string>& asset_rels, bool keep);
  const std::string& project_dir() const { return dir_; }
  std::string abs(const std::string& rel) const;

  // Walk the project: create missing .meta files, notice moves / renames /
  // copies / content changes / deletions. Does not touch references; call
  // fix_references(report) for that (after saving open scenes).
  ScanReport scan();
  // Rewrite references for every move in the report. Returns how many
  // strings changed.
  std::size_t fix_references(const ScanReport& report);

  const std::vector<Entry>& entries() const { return entries_; }
  const Entry* by_path(const std::string& rel) const;
  const Entry* by_guid(const std::string& guid) const;
  std::string path_of(const std::string& guid) const;  // "" when unknown
  // Folders that hold assets or were created for them (project-relative).
  std::vector<std::string> folders() const;

  // Import settings: written to the .meta, then reimported.
  bool set_settings(const std::string& rel, const json::Value& settings,
                    std::string* error = nullptr);
  bool reimport(const std::string& rel);

  // Every project file (.json / .lua / .ui / .anim ...) mentioning `rel`.
  std::vector<Ref> find_references(const std::string& rel) const;
  std::size_t rewrite_references(const std::string& old_rel, const std::string& new_rel) const;

  // Rename / move a file and its .meta (GUID kept) and rewrite references.
  // A Source .mdl takes its .vvd / .vtx / .phy along (same base name).
  bool move(const std::string& old_rel, const std::string& new_rel, std::string* error = nullptr,
            std::size_t* refs_rewritten = nullptr);
  // Delete a file and its .meta (a .mdl: its .vvd / .vtx / .phy too).
  // Refuses while referenced unless force
  // (refs_out receives the references either way).
  bool remove(const std::string& rel, bool force, std::vector<Ref>* refs_out = nullptr,
              std::string* error = nullptr);
  bool make_folder(const std::string& rel, std::string* error = nullptr);

  static bool ignored_dir(const std::string& name);

 private:
  void import_entry(Entry* e);
  void sort_entries();
  void adopt_sidecars(const std::vector<std::string>& sidecar_rels, MigrationReport* rep);
  void log(const std::string& line) const;
  void prune_meta_dirs(const std::string& asset_rel) const;

  std::string dir_;
  std::vector<Entry> entries_;
  std::vector<std::string> extra_folders_;
  Importer importer_;
  MigrationReport migration_;
};

}  // namespace assetdb
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
