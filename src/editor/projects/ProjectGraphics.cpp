#include "editor/projects/ProjectGraphics.h"

#include "editor/projects/ProjectStore.h"
#include "editor/ux/Actions.h"

#include <imgui.h>

#include <iostream>
#include <optional>

namespace ts {
namespace tombstone {
namespace editor {
namespace {
std::optional<ProjectInfo> g_pending;
}

void draw_project_settings_menu(ProjectInfo& project) {
  if (ux::MenuItem("Project settings...")) ImGui::OpenPopup("Project settings");
  if (!ImGui::BeginPopupModal("Project settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::TextUnformatted("Saved in project.json and shared with the team.");
  const char* items[] = {"Use engine default", "OpenGL", "Vulkan", "Direct3D 12"};
  int g = 0;
  if (project.graphics_api == "gl") g = 1;
  else if (project.graphics_api == "vk") g = 2;
  else if (project.graphics_api == "dx12") g = 3;
  ImGui::SetNextItemWidth(260.0f);
  if (ImGui::Combo("Graphics API", &g, items, 4)) {
    project.graphics_api = g == 1 ? "gl" : g == 2 ? "vk" : g == 3 ? "dx12" : "";
    std::string err;
    if (!ProjectStore::write_project_json(project, &err)) {
      std::cerr << "[gfx] could not save project graphics API: " << err << '\n';
    } else {
      std::cout << "[gfx] project graphics API saved ("
                << (project.graphics_api.empty() ? "engine default" : project.graphics_api) << ")\n";
      g_pending = project;
    }
  }
  ImGui::TextDisabled("Engine default is Editor Settings. --gfx= wins over both.");
  if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

bool take_graphics_relaunch(ProjectInfo* project) {
  if (!g_pending || !project) return false;
  *project = *g_pending;
  g_pending.reset();
  return true;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
