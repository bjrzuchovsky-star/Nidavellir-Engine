#include "editor/screens/LoadingScreen.h"

#include "editor/ui/Brand.h"
#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <iostream>

namespace ts {
namespace tombstone {
namespace editor {

void LoadingScreen::on_enter() {
  elapsed_seconds_ = 0.0f;
  std::cout << "[Loading] splash / asset warm-up\n";
}

void LoadingScreen::on_exit() {
  std::cout << "[Loading] done\n";
}

AppState LoadingScreen::on_update(float delta_seconds) {
  elapsed_seconds_ += delta_seconds;
  const float progress =
      std::clamp(elapsed_seconds_ / kMinSplashSeconds, 0.0f, 1.0f);

  if (ImGui::GetCurrentContext() != nullptr) {
    theme::BeginRoot("##LoadingSplash");

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float card_w = std::min(avail.x * 0.6f, 540.0f);
    const float card_h = 330.0f;
    ImGui::SetCursorPos(ImVec2((avail.x - card_w) * 0.5f,
                               std::max(12.0f, (avail.y - card_h) * 0.42f)));
    theme::BeginCard("##LoadingCard", card_w, card_h, ImGuiChildFlags_Borders,
                     ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse);

    const float inner_w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    theme::BrandHero(brand::kProduct, brand::kTagline, inner_w);

    ImGui::Dummy(ImVec2(0.0f, 22.0f));
    const float bar_w = std::min(inner_w, 400.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner_w - bar_w) * 0.5f);
    theme::BrandProgress(progress, ImVec2(bar_w, 8.0f));

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    char status[96];
    std::snprintf(status, sizeof(status), "%s...  %d%%",
                  brand::LoadingLine(progress),
                  static_cast<int>(progress * 100.0f));
    theme::CenteredText(status, ImGui::GetFontSize(), theme::TextMuted(),
                        inner_w);

    // Footer line pinned to the bottom of the card.
    const char* footer = "NIDAVELLIR ENGINE  //  WHERE GAMES ARE FORGED";
    const float footer_y =
        ImGui::GetWindowHeight() - ImGui::GetTextLineHeight() - 18.0f;
    if (ImGui::GetCursorPosY() < footer_y) {
      ImGui::SetCursorPosY(footer_y);
    }
    theme::CenteredText(footer, ImGui::GetFontSize(), theme::CopperMuted(),
                        inner_w);

    theme::EndCard();
    theme::EndRoot();
  }

  if (elapsed_seconds_ >= kMinSplashSeconds) {
    return AppState::Login;
  }
  return AppState::Loading;
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
