#pragma once

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

// Picked when the project is staked; stored as "dimension" in project.json
// ("2d" / "3d" / "vr"). Projects from before the picker are 2D.
enum class ProjectKind {
  TwoD,
  ThreeD,
  VR,
};

// On-disk project descriptor (folder under projects_root + project.json).
struct ProjectInfo {
  std::string id;              // folder name under projects_root
  std::string name;            // display name from project.json
  ProjectKind kind = ProjectKind::TwoD;
  std::string path;            // absolute or cwd-relative project folder
  std::string created;         // ISO-ish timestamp string
  std::string last_opened;     // optional; empty if never opened
  // Enabled plugin ids ("plugins": "a,b" in project.json). plugins_set is
  // false for projects written before plugins existed: they get the
  // default built-ins for their kind until somebody changes the list.
  std::vector<std::string> plugins;
  bool plugins_set = false;
};

inline const char* to_string(ProjectKind kind) {
  switch (kind) {
    case ProjectKind::TwoD:
      return "2D";
    case ProjectKind::ThreeD:
      return "3D";
    case ProjectKind::VR:
      return "VR";
  }
  return "Unknown";
}

inline const char* dimension_string(ProjectKind kind) {
  switch (kind) {
    case ProjectKind::TwoD:
      return "2d";
    case ProjectKind::ThreeD:
      return "3d";
    case ProjectKind::VR:
      return "vr";
  }
  return "2d";
}

// Unknown / missing values read as 2D (backward compatible).
inline ProjectKind project_kind_from_dimension(const std::string& dimension) {
  if (dimension == "3d" || dimension == "3D") {
    return ProjectKind::ThreeD;
  }
  if (dimension == "vr" || dimension == "VR") {
    return ProjectKind::VR;
  }
  return ProjectKind::TwoD;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
