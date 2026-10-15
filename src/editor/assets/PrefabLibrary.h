#pragma once

// Editor-side prefab cache shared by the 2D and 3D editors: loads
// .prefab.json assets on demand, notices files changed on disk (another
// editor, a text editor, git) and keeps asset edits undoable alongside the
// scene's undo history.

#include "scene/Prefab.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

class PrefabLibrary {
 public:
  void set_project(const std::string& project_dir);
  const std::string& project_dir() const { return dir_; }
  std::string abs_path(const std::string& rel) const;

  // Cached asset (loaded on first use); nullptr when it does not load.
  const prefab::Asset* get(const std::string& rel);
  prefab::AssetLookup lookup();
  // Write + cache. Records the edit for undo when undo_depth is set (the
  // scene's undo depth right after the matching step was pushed).
  bool save(const std::string& rel, const prefab::Asset& asset, std::string* error,
            std::size_t undo_depth = 0, const std::string& label = {});
  // Assets whose files changed since they were cached (reloaded now).
  std::vector<std::string> poll_changes();
  // Every prefab in the project (rescanned each call).
  std::vector<std::string> list() const;
  void clear() { cache_.clear(); undo_.clear(); redo_.clear(); }

  // Asset side of undo / redo. depth = scene undo depth before the undo /
  // after the redo. True when an asset file was rewritten.
  bool on_undo(std::size_t depth, const std::string& label);
  bool on_redo(std::size_t depth, const std::string& label);
  void on_new_step() { redo_.clear(); }

 private:
  struct Entry {
    prefab::Asset asset;
    std::filesystem::file_time_type mtime{};
    bool ok = false;
  };
  struct Edit {
    std::string rel;
    std::string before;  // "" = the file did not exist
    std::string after;
    std::size_t depth = 0;
    std::string label;
  };
  bool load(const std::string& rel, Entry* e);
  bool write_text(const std::string& rel, const std::string& text);

  std::string dir_;
  std::map<std::string, Entry> cache_;
  std::vector<Edit> undo_;
  std::vector<Edit> redo_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
