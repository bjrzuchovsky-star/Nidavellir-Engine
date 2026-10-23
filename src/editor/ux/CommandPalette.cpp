#include "editor/ux/CommandPalette.h"

#include "editor/ui/Theme.h"
#include "editor/ux/Tooltip.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace ts::tombstone::editor::ux {

namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool word_start(std::string_view t, std::size_t i) {
  if (i == 0) return true;
  const char p = t[i - 1];
  return p == ' ' || p == '/' || p == '>' || p == '_' || p == '-' || p == '(' || p == '.';
}

std::string display_path(const std::string& p) {
  std::string out;
  for (char c : p) {
    if (c == '/') out += " > ";
    else out += c;
  }
  return out;
}

}  // namespace

int fuzzy_score(std::string_view query, std::string_view text) {
  // Trim the query; empty matches everything with score 0.
  while (!query.empty() && query.front() == ' ') query.remove_prefix(1);
  while (!query.empty() && query.back() == ' ') query.remove_suffix(1);
  if (query.empty()) return 0;
  // Whole-substring hit beats a scattered one.
  std::string lt(text.size(), ' '), lq(query.size(), ' ');
  for (std::size_t i = 0; i < text.size(); ++i) lt[i] = lower(text[i]);
  for (std::size_t i = 0; i < query.size(); ++i) lq[i] = lower(query[i]);
  int score = 0;
  const std::size_t sub = lt.find(lq);
  if (sub != std::string::npos) {
    score += 1000 + (word_start(lt, sub) ? 400 : 0) - static_cast<int>(sub);
  }
  // Every query character in order (spaces in the query are free).
  std::size_t ti = 0;
  int run = 0;
  for (char qc : lq) {
    if (qc == ' ') {
      run = 0;
      continue;
    }
    bool found = false;
    while (ti < lt.size()) {
      if (lt[ti] == qc) {
        score += 10 + run * 15 + (word_start(lt, ti) ? 30 : 0);
        ++run;
        ++ti;
        found = true;
        break;
      }
      run = 0;
      ++ti;
    }
    if (!found) return -1;
  }
  return score - static_cast<int>(text.size() / 8);
}

std::string palette_haystack(const Action& a) {
  std::string h = a.label;
  if (!a.path.empty()) h = display_path(a.path) + " > " + h;
  if (!a.keywords.empty()) h += "  " + a.keywords;
  return h;
}

std::vector<std::size_t> palette_rank(std::string_view query, const std::vector<Action>& actions) {
  struct Hit {
    std::size_t index;
    int score;
  };
  std::vector<Hit> hits;
  for (std::size_t i = 0; i < actions.size(); ++i) {
    const Action& a = actions[i];
    // Label hits count double so "save" finds Save before "Autosave note".
    int s = fuzzy_score(query, palette_haystack(a));
    if (s < 0) continue;
    const int ls = fuzzy_score(query, a.label);
    if (ls > 0) s += ls;
    if (!a.enabled) s -= 100000;
    hits.push_back({i, s});
  }
  std::stable_sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.score > y.score; });
  std::vector<std::size_t> out;
  out.reserve(hits.size());
  for (const Hit& h : hits) out.push_back(h.index);
  return out;
}

void CommandPalette::open() {
  open_ = true;
  want_open_ = true;
  focus_input_ = true;
  highlight_ = 0;
  query_.clear();
  buf_[0] = '\0';
}

void CommandPalette::set_query(const std::string& q) {
  query_ = q;
  std::snprintf(buf_, sizeof(buf_), "%s", q.c_str());
  highlight_ = 0;
  reload_input_ = true;  // an active InputText keeps its own copy otherwise
}

std::string CommandPalette::draw(const std::vector<Action>& actions) {
  if (!open_) return {};
  const char* kId = "Command Palette###ux_palette";
  if (want_open_) {
    ImGui::OpenPopup(kId);
    want_open_ = false;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float w = std::min(640.0f, vp->WorkSize.x - 40.0f);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f, vp->WorkPos.y + 60.0f));
  ImGui::SetNextWindowSize(ImVec2(w, 0.0f));
  std::string chosen;
  if (!ImGui::BeginPopup(kId, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
    open_ = false;
    return {};
  }
  ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
  ImGui::TextUnformatted("Command Palette");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("  Enter runs  |  Up/Down picks  |  Esc closes");
  if (reload_input_) {
    if (ImGui::GetActiveID() == ImGui::GetID("##ux_palette_q")) ImGui::ClearActiveID();
    reload_input_ = false;
    focus_input_ = true;
  }
  if (focus_input_) {
    ImGui::SetKeyboardFocusHere();
    focus_input_ = false;
  }
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::InputTextWithHint("##ux_palette_q", "Type a command, panel, object or setting...",
                               buf_, sizeof(buf_))) {
    query_ = buf_;
    highlight_ = 0;
  }
  const std::vector<std::size_t> rows = palette_rank(query_, actions);
  const int n = static_cast<int>(rows.size());
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) highlight_ = std::min(n - 1, highlight_ + 1);
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) highlight_ = std::max(0, highlight_ - 1);
  if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) highlight_ = std::min(n - 1, highlight_ + 10);
  if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) highlight_ = std::max(0, highlight_ - 10);
  highlight_ = std::clamp(highlight_, 0, std::max(0, n - 1));
  const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

  ImGui::Separator();
  const float row_h = ImGui::GetTextLineHeightWithSpacing();
  ImGui::BeginChild("##ux_palette_rows", ImVec2(0, row_h * std::min(14, std::max(1, n)) + 6.0f));
  if (n == 0) {
    ImGui::TextDisabled("Nothing matches \"%s\". Try fewer letters, or a menu name like \"view\".",
                        query_.c_str());
  }
  ImGuiListClipper clip;
  clip.Begin(n, row_h);
  if (highlight_ >= 0 && n > 0) clip.IncludeItemByIndex(highlight_);
  while (clip.Step()) {
    for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
      const Action& a = actions[rows[static_cast<std::size_t>(r)]];
      ImGui::PushID(r);
      std::string label = a.label;
      if (a.checkable) label = std::string(a.checked ? "[x] " : "[ ] ") + label;
      const std::string path = display_path(a.path);
      ImGui::BeginDisabled(!a.enabled);
      const bool sel = r == highlight_;
      if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowOverlap) && a.enabled) chosen = a.id;
      if (sel && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
      if (sel && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
        ImGui::SetScrollHereY();
      if (ImGui::IsItemHovered() && ImGui::GetIO().MouseDelta.x != 0.0f) highlight_ = r;
      ImGui::SameLine(8.0f);
      if (!path.empty()) {
        ImGui::TextDisabled("%s >", path.c_str());
        ImGui::SameLine();
      }
      ImGui::TextUnformatted(label.c_str());
      if (!a.enabled) {
        ImGui::SameLine();
        ImGui::TextDisabled("(not available right now)");
      }
      if (!a.shortcut.empty()) {
        const float sw = ImGui::CalcTextSize(a.shortcut.c_str()).x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - sw - 6.0f);
        ImGui::TextDisabled("%s", a.shortcut.c_str());
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
  if (n > 0) {
    const Action& h = actions[rows[static_cast<std::size_t>(highlight_)]];
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    if (!h.tip.empty()) ImGui::TextDisabled("%s", h.tip.c_str());
    else ImGui::TextDisabled("%s", palette_haystack(h).c_str());
    ImGui::PopTextWrapPos();
    if (enter && h.enabled) chosen = h.id;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
    open_ = false;
  }
  if (!chosen.empty()) {
    ImGui::CloseCurrentPopup();
    open_ = false;
  }
  ImGui::EndPopup();
  return chosen;
}

}  // namespace ts::tombstone::editor::ux
