#include "editor/ux/Walkthrough.h"

#include "editor/ui/Theme.h"
#include "editor/ux/UxPrefs.h"
#include "editor/ux/UxStrings.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace ts::tombstone::editor::ux {

namespace {
bool g_autostart = true;
}
void Walkthrough::set_autostart(bool on) { g_autostart = on; }
bool Walkthrough::autostart() { return g_autostart; }

int Walkthrough::step_count() const { return static_cast<int>(walkthrough_steps(three_d_).size()); }

bool Walkthrough::seen() const { return three_d_ ? prefs().walkthrough_3d_done : prefs().walkthrough_2d_done; }

void Walkthrough::start() {
  active_ = true;
  step_ = 0;
  shown_step_ = -1;
  dont_show_again = true;
}

void Walkthrough::stop(bool remember) {
  active_ = false;
  if (remember && !seen()) {
    (three_d_ ? prefs().walkthrough_3d_done : prefs().walkthrough_2d_done) = true;
    persist();
  }
}

void Walkthrough::next() {
  if (step_ + 1 >= step_count()) {
    stop(true);  // finishing counts as seen
    return;
  }
  ++step_;
}

void Walkthrough::back() {
  if (step_ > 0) --step_;
}

void Walkthrough::frame(const std::function<void(const std::string&)>& show_panel) {
  ++frames_;
  if (!active_ && !auto_started_ && g_autostart && !seen() && frames_ > 3) {
    auto_started_ = true;
    start();
  }
  if (!active_) return;
  const std::vector<WalkStep>& steps = walkthrough_steps(three_d_);
  if (steps.empty()) {
    active_ = false;
    return;
  }
  step_ = std::clamp(step_, 0, static_cast<int>(steps.size()) - 1);
  const WalkStep& st = steps[static_cast<std::size_t>(step_)];
  if (shown_step_ != step_) {
    shown_step_ = step_;
    if (*st.panel && show_panel) show_panel(st.panel);
  }

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImRect target(vp->WorkPos, ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y));
  bool pointed = false;
  if (*st.window) {
    if (ImGuiWindow* w = ImGui::FindWindowByName(st.window); w && (w->Active || w->WasActive) && !w->Hidden) {
      target = ImRect(w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y));
      pointed = true;
    }
  }

  // Dim everything but the panel being explained; outline it in amber.
  ImDrawList* dl = ImGui::GetForegroundDrawList(const_cast<ImGuiViewport*>(vp));
  const ImU32 dim = IM_COL32(0, 0, 0, 120);
  const ImVec2 a = vp->Pos, b(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
  if (pointed) {
    dl->AddRectFilled(a, ImVec2(b.x, target.Min.y), dim);
    dl->AddRectFilled(ImVec2(a.x, target.Max.y), b, dim);
    dl->AddRectFilled(ImVec2(a.x, target.Min.y), ImVec2(target.Min.x, target.Max.y), dim);
    dl->AddRectFilled(ImVec2(target.Max.x, target.Min.y), ImVec2(b.x, target.Max.y), dim);
    dl->AddRect(target.Min, target.Max, theme::U32(theme::Accent()), 3.0f, 0, 3.0f);
  } else {
    dl->AddRectFilled(a, b, dim);
  }

  // The card: beside the panel when there's room, else centred.
  const float card_w = 380.0f;
  ImVec2 pos(vp->WorkPos.x + (vp->WorkSize.x - card_w) * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.30f);
  if (pointed) {
    const float gap = 14.0f;
    if (target.Max.x + gap + card_w < vp->WorkPos.x + vp->WorkSize.x) pos = ImVec2(target.Max.x + gap, target.Min.y + 40.0f);
    else if (target.Min.x - gap - card_w > vp->WorkPos.x) pos = ImVec2(target.Min.x - gap - card_w, target.Min.y + 40.0f);
    else pos = ImVec2(target.GetCenter().x - card_w * 0.5f, target.GetCenter().y - 80.0f);
    pos.y = std::clamp(pos.y, vp->WorkPos.y + 8.0f, vp->WorkPos.y + vp->WorkSize.y - 260.0f);
  }
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(card_w, 0), ImGuiCond_Always);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleColor(ImGuiCol_Border, theme::Accent());
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::Begin("###ts_walkthrough", nullptr, flags)) {
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
    ImGui::Text("%s tour  -  step %d of %d", three_d_ ? "3D editor" : "2D editor", step_ + 1, step_count());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
    ImGui::TextUnformatted(st.title);
    ImGui::PopStyleColor();
    ImGui::PushTextWrapPos(card_w - 32.0f);
    ImGui::TextUnformatted(st.body);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    // Progress pips.
    {
      ImDrawList* wdl = ImGui::GetWindowDrawList();
      ImVec2 p = ImGui::GetCursorScreenPos();
      for (int i = 0; i < step_count(); ++i) {
        wdl->AddCircleFilled(ImVec2(p.x + 5.0f + i * 14.0f, p.y + 5.0f), 4.0f,
                             theme::U32(i == step_ ? theme::Accent() : theme::TextMuted(), i == step_ ? 1.0f : 0.5f));
      }
      ImGui::Dummy(ImVec2(step_count() * 14.0f, 10.0f));
    }
    ImGui::Spacing();
    ImGui::Checkbox("Don't show again", &dont_show_again);
    ImGui::Spacing();
    const bool last = step_ + 1 >= step_count();
    bool go_next = false, go_back = false, skip = false;
    ImGui::BeginDisabled(step_ == 0);
    go_back = theme::SecondaryButton("Back");
    ImGui::EndDisabled();
    ImGui::SameLine();
    go_next = theme::PrimaryButton(last ? "Finish" : "Next");
    ImGui::SameLine();
    skip = theme::SecondaryButton("Skip tour");
    ImGui::SameLine();
    ImGui::TextDisabled("Esc skips");
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) skip = true;
    if (go_back) back();
    if (skip) stop(dont_show_again);
    else if (go_next) next();
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

}  // namespace ts::tombstone::editor::ux
