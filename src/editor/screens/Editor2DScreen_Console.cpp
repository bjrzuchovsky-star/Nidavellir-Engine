// The Telegraph: the editor's console. World lines from the ride (script
// log / warn / errors with file:line, toasts, trigger events) and the
// editor's own status notes, with level toggles, search, clear,
// auto-scroll, click-to-select and a status-bar badge.

#include "editor/screens/Editor2DScreen.h"
#include "editor/ux/Tooltip.h"
#include "editor/plugins/ProfilerPanels.h"

#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace {

ImVec4 level_color(runtime::LogLevel level) {
  switch (level) {
    case runtime::LogLevel::Warn:
      return theme::Warning();
    case runtime::LogLevel::Error:
      return theme::Danger();
    case runtime::LogLevel::Info:
    default:
      return theme::Text();
  }
}

}  // namespace

std::size_t Editor2DScreen::pump_play_logs() {
  if (!is_playing()) {
    return 0;
  }
  std::vector<runtime::LogEntry> lines = play_.world().take_logs();
  for (runtime::LogEntry& e : lines) {
    std::cout << "[Telegraph] " << runtime::format_log(e, true) << '\n';
    telegraph_.add(std::move(e));
  }
  return lines.size();
}

bool Editor2DScreen::focus_log_entry(const runtime::LogEntry& entry) {
  if (entry.entity == 0) {
    return false;
  }
  const Entity2D* e = workspace_.find(entry.entity);
  if (!e) {
    note("That one only rode in the Play session (spawned, then gone).");
    return false;
  }
  workspace_.select(e->id);
  show_inspector_ = true;
  if (!is_playing()) {
    // Frame it in the edit viewport.
    workspace_.set_pan(e->center_x(), e->center_y());
  }
  note("Picked out " + e->name);
  return true;
}

void Editor2DScreen::draw_console() {
  TelegraphLog& log = telegraph_;
  // Toolbar: level toggles with counts, search, clear, auto-scroll.
  char label[48];
  std::snprintf(label, sizeof(label), "Info %zu###tg_info",
                log.count(runtime::LogLevel::Info));
  theme::ToggleButton(label, &log.show_info, ImVec2(78, 0));
  ImGui::SameLine();
  std::snprintf(label, sizeof(label), "Warn %zu###tg_warn",
                log.count(runtime::LogLevel::Warn));
  theme::ToggleButton(label, &log.show_warn, ImVec2(78, 0));
  ImGui::SameLine();
  std::snprintf(label, sizeof(label), "Error %zu###tg_error",
                log.count(runtime::LogLevel::Error));
  theme::ToggleButton(label, &log.show_error, ImVec2(84, 0));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(
      std::max(80.0f, ImGui::GetContentRegionAvail().x - 160.0f));
  if (ImGui::InputTextWithHint("##tg_search", "Search the wire...",
                               telegraph_search_, sizeof(telegraph_search_))) {
    log.search = telegraph_search_;
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Clear", ImVec2(56, 0))) {
    log.clear();
  }
  ImGui::SameLine();
  ImGui::Checkbox("Auto-scroll", &telegraph_autoscroll_);

  ImGui::Separator();
  const std::vector<std::size_t> rows = log.visible();
  const bool cmd_line = plugins_.loaded("ts.profiler");
  ImGui::BeginChild("##tg_lines",
                    ImVec2(0, cmd_line ? -ImGui::GetFrameHeightWithSpacing() : 0.0f),
                    ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar);
  if (rows.empty()) {
    ImGui::TextDisabled(log.size() == 0
                            ? "Quiet on the wire. Press Play and the scripts "
                              "will report in here."
                            : "Nothing matches the filter.");
  }
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(rows.size()));
  while (clipper.Step()) {
    for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
      const TelegraphLog::Line& line = log.lines()[rows[static_cast<std::size_t>(r)]];
      const runtime::LogEntry& e = line.entry;
      ImGui::PushID(static_cast<int>(line.seq & 0x7fffffff));
      ImGui::PushStyleColor(ImGuiCol_Text, level_color(e.level));
      const std::string text = runtime::format_log(e, true);
      const bool clicked = ImGui::Selectable(text.c_str(), false,
                                             ImGuiSelectableFlags_None);
      ImGui::PopStyleColor();
      if (e.entity != 0 && ImGui::IsItemHovered()) {
        const Entity2D* who = workspace_.find(e.entity);
        ux::SetTooltip("%s", who ? ("Click to pick out " + who->name).c_str()
                                    : "Spawned during the ride");
      }
      if (clicked) {
        focus_log_entry(e);
      }
      ImGui::PopID();
    }
  }
  // Follow new lines unless the reader scrolled up.
  if (telegraph_autoscroll_ && log.total() != telegraph_seen_total_ &&
      ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f) {
    ImGui::SetScrollHereY(1.0f);
  }
  telegraph_seen_total_ = log.total();
  ImGui::EndChild();
  if (cmd_line) {
    std::string echo, out;
    if (draw_console_line(telegraph_console_, &echo, &out)) {
      log.add(runtime::LogLevel::Info, "console", echo);
      if (!out.empty()) log.add(runtime::LogLevel::Info, "console", out);
    }
  }
  log.mark_seen();  // the panel is on screen: the badge resets
}

void Editor2DScreen::draw_telegraph_badge() {
  const std::size_t errors = telegraph_.unseen(runtime::LogLevel::Error);
  const std::size_t warns = telegraph_.unseen(runtime::LogLevel::Warn);
  char badge[64];
  if (errors + warns == 0) {
    std::snprintf(badge, sizeof(badge), "Telegraph###tg_badge");
  } else if (errors > 0) {
    std::snprintf(badge, sizeof(badge), "Telegraph: %zu error%s###tg_badge",
                  errors, errors == 1 ? "" : "s");
  } else {
    std::snprintf(badge, sizeof(badge), "Telegraph: %zu warning%s###tg_badge",
                  warns, warns == 1 ? "" : "s");
  }
  const ImVec4 col = errors > 0  ? theme::Danger()
                     : warns > 0 ? theme::Warning()
                                 : theme::TextMuted();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col.x, col.y, col.z, 0.18f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(col.x, col.y, col.z, 0.32f));
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 0.0f));
  if (ImGui::SmallButton(badge)) {
    show_console_ = true;
    focus_console_ = true;
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  if (ImGui::IsItemHovered()) {
    ux::SetTooltip("Open the Telegraph (script log, toasts, trigger "
                      "events, editor notes)");
  }
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
