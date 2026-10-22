#include "editor/ux/EmptyState.h"

#include "editor/ui/Theme.h"
#include "editor/ux/Tooltip.h"
#include "editor/ux/UxStrings.h"

#include <imgui.h>

namespace ts {
namespace tombstone {
namespace editor {
namespace ux {

namespace {
std::set<std::string> g_seen;
}

bool EmptyState(const char* id, std::initializer_list<EmptyButton> buttons, const char* body_override) {
  g_seen.insert(id);
  const EmptyText t = empty_text(id);
  ImGui::PushID(id);
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
  ImGui::TextUnformatted(t.title);
  ImGui::PopStyleColor();
  const char* body = body_override ? body_override : t.body;
  if (body && *body) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
    ImGui::TextUnformatted(body);
    ImGui::PopStyleColor();
  }
  ImGui::PopTextWrapPos();
  bool ran = false;
  bool first = true;
  float line_w = 0.0f;
  const float avail = ImGui::GetContentRegionAvail().x;
  for (const EmptyButton& b : buttons) {
    const float w = ImGui::CalcTextSize(b.label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    if (!first && line_w + w + ImGui::GetStyle().ItemSpacing.x < avail) ImGui::SameLine();
    else line_w = 0.0f;
    line_w += w + ImGui::GetStyle().ItemSpacing.x;
    ImGui::BeginDisabled(!b.enabled);
    const bool hit = first ? theme::PrimaryButton(b.label) : theme::SecondaryButton(b.label);
    ImGui::EndDisabled();
    if (b.tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) SetTooltip("%s", b.tip);
    if (hit && b.run) {
      b.run();
      ran = true;
    }
    first = false;
  }
  ImGui::PopID();
  return ran;
}

const std::set<std::string>& empty_states_seen() { return g_seen; }
void reset_empty_states_seen() { g_seen.clear(); }

}  // namespace ux
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
