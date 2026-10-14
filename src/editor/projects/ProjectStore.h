#pragma once

#include "editor/ProjectInfo.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

// Scans projects_root for project folders (each containing project.json).
// Creates new projects (2D / 3D / VR) on disk and seeds a sample when the root is empty.
class ProjectStore {
 public:
  explicit ProjectStore(std::string projects_root);

  const std::string& projects_root() const { return projects_root_; }
  void set_projects_root(std::string projects_root);

  const std::vector<ProjectInfo>& projects() const { return projects_; }
  const std::string& last_error() const { return last_error_; }

  // Ensures root exists, scans for project.json folders, seeds sample if empty.
  // Returns false on hard failures (missing/unwritable root).
  bool refresh();

  // Validate a display name before create. Fills error_out on failure.
  // Rules: non-empty (after trim), length <= 64, safe folder characters only
  // (letters, digits, space, dash, underscore, period), no path separators /
  // reserved Windows chars, and resulting slug must be usable.
  static bool validate_project_name(const std::string& name,
                                    std::string* error_out = nullptr);

  // Creates <root>/<slug>/project.json for a new 2D project. Refreshes list.
  // Fails on invalid name or if the slug folder already exists (no auto-rename).
  bool create_project_2d(const std::string& name, ProjectInfo* out = nullptr);
  // Same for any kind. plugins (when given) is written as the enabled
  // list; null leaves "plugins" out (= defaults for the kind).
  bool create_project(const std::string& name, ProjectKind kind,
                      const std::vector<std::string>* plugins,
                      ProjectInfo* out = nullptr);

  // Marks last_opened on disk and in the in-memory list.
  bool touch_last_opened(const std::string& project_path);

  // Deletes a project folder from disk (project.json + contents) and refreshes.
  // project_path must be under projects_root. Refuses paths outside the root.
  bool delete_project(const std::string& project_path);

  // Load a single project.json from a folder (no list mutation).
  static bool load_project_json(const std::string& project_dir,
                                ProjectInfo* out,
                                std::string* error_out = nullptr);

  static bool write_project_json(const ProjectInfo& project,
                                 std::string* error_out = nullptr);

  static std::string make_slug(const std::string& name);

  // "plugins" field codec: "a, b,c" <-> {"a","b","c"} (trimmed, deduped).
  static std::vector<std::string> split_plugin_ids(const std::string& text);
  static std::string join_plugin_ids(const std::vector<std::string>& ids);

 private:
  static std::string now_timestamp();
  static std::string trim_copy(const std::string& s);
  bool seed_sample_2d_if_empty();

  std::string projects_root_;
  std::vector<ProjectInfo> projects_;
  std::string last_error_;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
