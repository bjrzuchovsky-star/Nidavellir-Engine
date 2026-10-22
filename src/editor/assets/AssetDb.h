#pragma once

// Project asset database: every importable file (images, 3D models of any
// supported format - glTF, Source .mdl, FBX / OBJ / DAE... see
// render3d/ModelImport.h - sounds, fonts) gets a sidecar "<file>.meta" holding a stable GUID, its
// import settings and what the importer found. The GUID is the asset's
// identity: when a file is renamed or moved (in the Asset Browser, in
// Explorer together with its .meta, or even without the .meta - matched by
// content hash) the database notices on the next scan and rewrites every
// reference to the old path in the project's scenes, prefabs and other
// JSON / Lua files, so nothing breaks. No UI (AssetBrowser draws it).
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
    std::vector<std::string> orphan_metas;  // .meta without its file
    bool any() const {
      return !created.empty() || !changed.empty() || !removed.empty() || !moved.empty() ||
             !duplicated.empty();
    }
  };

  void set_importer(Importer importer) { importer_ = std::move(importer); }
  void open(const std::string& project_dir);
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

  // Rename / move a file with its .meta (GUID kept) and rewrite references.
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

  std::string dir_;
  std::vector<Entry> entries_;
  std::vector<std::string> extra_folders_;
  Importer importer_;
};

}  // namespace assetdb
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
