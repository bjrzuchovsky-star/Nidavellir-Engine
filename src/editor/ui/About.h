#pragma once

// Help > About. The lines are the same strings as src/core/Brand.h.

#include "core/Brand.h"
#include "editor/ux/Actions.h"

#include <imgui.h>

namespace ts {
namespace tombstone {
namespace editor {
namespace about {

inline bool& open_flag() {
  static bool open = false;
  return open;
}

inline void draw_menu_item() {
  if (ux::MenuItem("About")) open_flag() = true;
}

inline void draw_popup() {
  if (open_flag()) {
    ImGui::OpenPopup("##AboutNidavellir");
    open_flag() = false;
  }
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
