#pragma once

// Help > About. The lines are the same strings as src/core/Brand.h.

#include "core/Brand.h"

#include <imgui.h>

namespace ts {
namespace tombstone {
namespace editor {
namespace about {

inline void draw_menu_item() {
  if (ImGui::MenuItem("About")) ImGui::OpenPopup("##AboutNidavellir");
  if (ImGui::BeginPopupModal("##AboutNidavellir", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(brand::kProduct);
    ImGui::TextUnformatted(brand::kCompany);
    ImGui::TextDisabled("%s", brand::kTagline);
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

}  // namespace about
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
