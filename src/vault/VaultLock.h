#pragma once

// A project's record of what came from (or went to) a Vault:
// <project>/.vault/lock.json. Commit it with the project. The dot folder
// keeps it out of the Asset Browser's reference scan and out of exports.
//
//   {"vault_lock": 1, "items": [{"item": "<id>", "name": "crate", "kind": "model",
//     "origin": "pulled" | "pushed", "library": "local" | "<server url>",
//     "version": 2, "pinned": false, "main": "assets/props/crate.gltf",
//     "guid": "<main GUID>", "deps": ["<texture item id>"], "when": "...",
//     "files": [{"path": "...", "sha256": "...", "guid": "..."}]}]}

#include "vault/VaultTypes.h"

#include <set>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace vault {

struct LockFile {
  std::string path;
  std::string sha256;
  std::string guid;
};

struct LockEntry {
  std::string item;
  std::string name;
  std::string kind;
  std::string origin = "pulled";
  std::string library;
  int version = 0;
  bool pinned = false;
  std::string main;
  std::string guid;
  std::vector<std::string> deps;
  std::string when;
  std::vector<LockFile> files;
};

class ProjectLock {
 public:
  static std::string lock_path(const std::string& project_dir);
  bool load(const std::string& project_dir, std::string* error = nullptr);  // missing file = empty
  bool save(std::string* error = nullptr) const;

  std::vector<LockEntry>& entries() { return entries_; }
  const std::vector<LockEntry>& entries() const { return entries_; }
  LockEntry* find(const std::string& item);
  const LockEntry* find(const std::string& item) const;
  const LockEntry* find_by_guid(const std::string& guid) const;
  const LockEntry* find_by_path(const std::string& rel) const;  // any of its files
  LockEntry& upsert(const LockEntry& e);
  void remove(const std::string& item);

 private:
  std::string dir_;
  std::vector<LockEntry> entries_;
};

// Exports: files of pulled Vault items nothing in the game uses. A pulled
// item counts as used when a scene, prefab, script, UI or other content
// file that is not itself an unused pulled file names one of its files
// (project-relative path); the items it depends on then count as used too.
// `content` is the export's candidate file list (project-relative; a
// .meta by its logical "<asset>.meta" name, read from the claim's
// .tombstone/meta folder); the result holds the pulled files to leave out
// (their .meta files included).
std::set<std::string> unused_pulled_files(const std::string& project_dir,
                                          const std::vector<std::string>& content);

}  // namespace vault
}  // namespace tombstone
}  // namespace ts
