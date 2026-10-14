#pragma once

namespace ts {
namespace tombstone {
namespace editor {

// High-level Admin UI flow. Screens map 1:1 onto these states so ImGui
// (or another UI backend) can replace the console stubs later.
enum class AppState {
  Loading,
  Login,
  ProjectManager,
  Settings,
  Editor2D,
  // VR projects: the survey camp (placeholder viewport + plugins).
  EditorFrontier,
  // 3D projects: the GL 3.3 editor.
  Editor3D,
  Quit,
};

inline const char* to_string(AppState state) {
  switch (state) {
    case AppState::Loading:
      return "Loading";
    case AppState::Login:
      return "Login";
    case AppState::ProjectManager:
      return "ProjectManager";
    case AppState::Settings:
      return "Settings";
    case AppState::Editor2D:
      return "Editor2D";
    case AppState::EditorFrontier:
      return "EditorFrontier";
    case AppState::Editor3D:
      return "Editor3D";
    case AppState::Quit:
      return "Quit";
  }
  return "Unknown";
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
