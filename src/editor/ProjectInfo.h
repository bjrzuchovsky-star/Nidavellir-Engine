#pragma once

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

// Picked when the project is staked; stored as "dimension" in project.json
// ("2d" / "3d"). Projects from before the picker are 2D. VR is a switch on a
// 3D project ("vr": true in project.json), not a dimension of its own:
// "dimension": "vr" from older builds loads as 3D with VR on (and is
// written back that way on the next save). ProjectKind::VR stays as a
// legacy spelling of "3D with VR on" for callers that still pass it;
// ProjectStore normalizes it to ThreeD + vr.
enum class ProjectKind {
  TwoD,
  ThreeD,
  VR,  // legacy: == ThreeD with vr = true
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
  // 3D only: VR on (headset preview, VR rig / grabbable inspectors, mirror,
  // ts_game rides in a headset when one is there, else on the desktop).
  bool vr = false;
};

// Does this project ride in VR? (3D with the VR switch on, or the legacy kind.)
inline bool vr_enabled(const ProjectInfo& p) {
  return p.kind == ProjectKind::VR || (p.kind == ProjectKind::ThreeD && p.vr);
}

// Folds the legacy VR kind into ThreeD + vr (and clears vr on 2D).
inline void normalize_vr(ProjectInfo& p) {
  if (p.kind == ProjectKind::VR) {
    p.kind = ProjectKind::ThreeD;
    p.vr = true;
  } else if (p.kind == ProjectKind::TwoD) {
    p.vr = false;
  }
}

// "2D", "3D" or "3D + VR" for badges and status lines.
inline const char* kind_label(const ProjectInfo& p) {
  if (p.kind == ProjectKind::TwoD) return "2D";
  return vr_enabled(p) ? "3D + VR" : "3D";
}

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
      return "3d";  // VR is "vr": true on a 3D project
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
