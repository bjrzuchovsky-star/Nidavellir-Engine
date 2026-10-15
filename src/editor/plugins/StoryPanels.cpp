#include "editor/plugins/StoryPanels.h"

#include "editor/ui/Theme.h"
#include "plugins/story/StoryPlugins.h"
#include "story/StoryRuntime.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {

constexpr float kNodeW = 210.0f;

int resize_cb(ImGuiInputTextCallbackData* d) {
  if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* s = static_cast<std::string*>(d->UserData);
    s->resize(static_cast<std::size_t>(d->BufTextLen));
    d->Buf = s->data();
  }
  return 0;
}

bool input_str(const char* label, std::string* s, bool multiline = false, float height = 0.0f) {
  const ImGuiInputTextFlags f = ImGuiInputTextFlags_CallbackResize;
  if (multiline) {
    return ImGui::InputTextMultiline(label, s->data(), s->capacity() + 1, ImVec2(-1, height), f, resize_cb, s);
  }
  return ImGui::InputText(label, s->data(), s->capacity() + 1, f, resize_cb, s);
}

// One string per line <-> a vector.
std::string join_lines(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& x : v) out += (out.empty() ? "" : "\n") + x;
  return out;
}

std::vector<std::string> split_lines(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty()) out.push_back(l);
  }
  return out;
}

bool edit_lines(const char* label, std::vector<std::string>* v, float height = 54.0f) {
  std::string t = join_lines(*v);
  if (input_str(label, &t, true, height)) {
    *v = split_lines(t);
    return true;
  }
  return false;
}

std::vector<std::string> scan_ext(const std::string& project, const char* sub, const char* ext) {
  std::vector<std::string> out;
  std::error_code ec;
  const fs::path dir = fs::path(project) / sub;
  if (project.empty() || !fs::is_directory(dir, ec)) return out;
  const std::string e = ext;
  for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
    const std::string n = f.path().filename().string();
    if (f.is_regular_file(ec) && n.size() > e.size() && n.substr(n.size() - e.size()) == e) {
      out.push_back(fs::relative(f.path(), project, ec).generic_string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool write_file(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << text;
  return f.good();
}

std::string project_dir_now() {
  plugins::StoryPlugin* p = plugins::StoryPlugin::instance();
  return p ? p->project_dir() : std::string();
}

story::StoryRuntime* live() {
  story::StoryRuntime* r = story::StoryRuntime::latest();
  return r && r->running() ? r : nullptr;
}

std::string unique_node_id(const story::Dialogue& d) {
  for (int i = 1;; ++i) {
    const std::string id = "node" + std::to_string(i);
    if (!d.node(id)) return id;
  }
}

std::string clip(const std::string& s, std::size_t n) {
  std::string one = s;
  std::replace(one.begin(), one.end(), '\n', ' ');
  return one.size() <= n ? one : one.substr(0, n - 3) + "...";
}

float node_height(const story::DialogueNode& n) { return 58.0f + 17.0f * static_cast<float>(n.choices.size()); }

// ---- Dialogue graph -----------------------------------------------------------------

void draw_graph(DialoguePanelState& s, const std::string& live_node) {
  ImGui::BeginChild("##dlg_canvas", ImVec2(ImGui::GetContentRegionAvail().x * 0.58f, 0), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const ImVec2 sz = ImGui::GetContentRegionAvail();
  dl->AddRectFilled(o, ImVec2(o.x + sz.x, o.y + sz.y), theme::U32(theme::CanvasBg()));
  for (float gx = std::fmod(s.pan[0], 32.0f); gx < sz.x; gx += 32.0f) {
    dl->AddLine(ImVec2(o.x + gx, o.y), ImVec2(o.x + gx, o.y + sz.y), theme::U32(theme::GridLine(), 0.5f));
  }
  for (float gy = std::fmod(s.pan[1], 32.0f); gy < sz.y; gy += 32.0f) {
    dl->AddLine(ImVec2(o.x, o.y + gy), ImVec2(o.x + sz.x, o.y + gy), theme::U32(theme::GridLine(), 0.5f));
  }
  ImGui::InvisibleButton("##dlg_bg", ImVec2(std::max(sz.x, 1.0f), std::max(sz.y, 1.0f)),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                             ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
    const ImVec2 d = ImGui::GetIO().MouseDelta;
    s.pan[0] += d.x;
    s.pan[1] += d.y;
  }
  auto at = [&](const story::DialogueNode& n) { return ImVec2(o.x + s.pan[0] + n.x, o.y + s.pan[1] + n.y); };
  // Links first.
  for (const story::DialogueNode& n : s.dlg.nodes) {
    const ImVec2 a = at(n);
    auto link = [&](const std::string& to, float y_off, ImU32 col, int label) {
      const story::DialogueNode* t = s.dlg.node(to);
      if (!t) return;
      const ImVec2 p0(a.x + kNodeW, a.y + y_off);
      const ImVec2 b = at(*t);
      const ImVec2 p1(b.x, b.y + 14.0f);
      const float dx = std::max(40.0f, std::fabs(p1.x - p0.x) * 0.5f);
      dl->AddBezierCubic(p0, ImVec2(p0.x + dx, p0.y), ImVec2(p1.x - dx, p1.y), p1, col, 2.0f);
      dl->AddCircleFilled(p1, 3.5f, col);
      if (label > 0) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%d", label);
        dl->AddText(ImVec2(p0.x + 4, p0.y - 14), col, buf);
      }
    };
    if (n.choices.empty()) {
      link(n.next, 14.0f, theme::U32(theme::Accent(), 0.9f), 0);
    } else {
      for (std::size_t i = 0; i < n.choices.size(); ++i) {
        link(n.choices[i].next, 50.0f + 17.0f * static_cast<float>(i), theme::U32(theme::Copper(), 0.9f),
             static_cast<int>(i + 1));
      }
    }
  }
  // Nodes.
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  int clicked = -1;
  for (std::size_t i = 0; i < s.dlg.nodes.size(); ++i) {
    const story::DialogueNode& n = s.dlg.nodes[i];
    const ImVec2 a = at(n);
    const ImVec2 b(a.x + kNodeW, a.y + node_height(n));
    const bool sel = static_cast<int>(i) == s.selected;
    const bool is_live = !live_node.empty() && n.id == live_node;
    const bool is_start = n.id == s.dlg.start;
    dl->AddRectFilled(a, b, theme::U32(theme::PanelBg()), 6.0f);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + 26), theme::U32(is_start ? theme::AccentMuted() : theme::HeaderBg()), 6.0f,
                      ImDrawFlags_RoundCornersTop);
    const ImVec4 edge = is_live ? theme::Success() : sel ? theme::Accent() : theme::Border();
    dl->AddRect(a, b, theme::U32(edge), 6.0f, 0, is_live || sel ? 2.5f : 1.0f);
    std::string head = n.id;
    if (!n.speaker.empty()) head += "  (" + n.speaker + ")";
    if (is_start) head += "  START";
    dl->AddText(ImVec2(a.x + 8, a.y + 5), theme::U32(theme::Text()), clip(head, 30).c_str());
    dl->AddText(ImVec2(a.x + 8, a.y + 32), theme::U32(theme::TextMuted()), clip(n.text, 30).c_str());
    for (std::size_t c = 0; c < n.choices.size(); ++c) {
      const std::string t = std::to_string(c + 1) + ". " + clip(n.choices[c].text, 26) +
                            (n.choices[c].condition.empty() ? "" : " ?");
      dl->AddText(ImVec2(a.x + 12, a.y + 50 + 17.0f * static_cast<float>(c) - 7), theme::U32(theme::Sand()), t.c_str());
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y &&
        mouse.y <= b.y) {
      clicked = static_cast<int>(i);
    }
  }
  if (clicked >= 0) {
    s.selected = clicked;
    s.dragging = clicked;
  } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    s.selected = -1;
  }
  if (s.dragging >= 0 && s.dragging < static_cast<int>(s.dlg.nodes.size())) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      const ImVec2 d = ImGui::GetIO().MouseDelta;
      if (d.x != 0 || d.y != 0) {
        s.dlg.nodes[static_cast<std::size_t>(s.dragging)].x += d.x;
        s.dlg.nodes[static_cast<std::size_t>(s.dragging)].y += d.y;
        s.dirty = true;
      }
    } else {
      s.dragging = -1;
    }
  }
  dl->AddText(ImVec2(o.x + 8, o.y + sz.y - 20), theme::U32(theme::TextMuted()),
              "drag nodes / RMB drag pans / amber = next, copper = choices");
  ImGui::EndChild();
}

void node_inspector(DialoguePanelState& s) {
  ImGui::BeginChild("##dlg_inspector", ImVec2(0, 0), ImGuiChildFlags_Borders);
  if (s.selected < 0 || s.selected >= static_cast<int>(s.dlg.nodes.size())) {
    theme::SectionHeader("Conversation", "start + speakers");
    if (input_str("Id##dlgid", &s.dlg.id)) s.dirty = true;
    if (input_str("Start node", &s.dlg.start)) s.dirty = true;
    ImGui::Spacing();
    std::string drop;
    for (auto& [k, sp] : s.dlg.speakers) {
      ImGui::PushID(k.c_str());
      ImGui::TextColored(theme::Accent(), "%s", k.c_str());
      if (input_str("Name", &sp.name)) s.dirty = true;
      if (input_str("Portrait", &sp.portrait)) s.dirty = true;
      if (theme::DangerButton("Remove speaker")) drop = k;
      ImGui::PopID();
      ImGui::Separator();
    }
    if (!drop.empty()) {
      s.dlg.speakers.erase(drop);
      s.dirty = true;
    }
    static std::string new_speaker = "stranger";
    input_str("##newspeaker", &new_speaker);
    ImGui::SameLine();
    if (theme::SecondaryButton("Add speaker") && !new_speaker.empty() && !s.dlg.speakers.count(new_speaker)) {
      s.dlg.speakers[new_speaker] = story::Speaker{new_speaker, ""};
      s.dirty = true;
    }
    ImGui::EndChild();
    return;
  }
  story::DialogueNode& n = s.dlg.nodes[static_cast<std::size_t>(s.selected)];
  theme::SectionHeader("Node", n.id.c_str());
  std::string id = n.id;
  if (input_str("Id", &id) && !id.empty() && !s.dlg.node(id)) {
    // Rename + follow links.
    for (story::DialogueNode& m : s.dlg.nodes) {
      if (m.next == n.id) m.next = id;
      for (story::DialogueChoice& c : m.choices) {
        if (c.next == n.id) c.next = id;
      }
    }
    if (s.dlg.start == n.id) s.dlg.start = id;
    n.id = id;
    s.dirty = true;
  }
  if (ImGui::BeginCombo("Speaker", n.speaker.empty() ? "(none)" : n.speaker.c_str())) {
    if (ImGui::Selectable("(none)", n.speaker.empty())) {
      n.speaker.clear();
      s.dirty = true;
    }
    for (const auto& [k, sp] : s.dlg.speakers) {
      if (ImGui::Selectable((k + "  -  " + sp.name).c_str(), n.speaker == k)) {
        n.speaker = k;
        s.dirty = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::TextColored(theme::TextMuted(), "Text ({var} fills in)");
  if (input_str("##text", &n.text, true, 70)) s.dirty = true;
  if (input_str("If", &n.condition)) s.dirty = true;
  ImGui::TextColored(theme::TextMuted(), "Actions on entry (one per line)");
  if (edit_lines("##nodeactions", &n.actions)) s.dirty = true;
  if (n.choices.empty() && input_str("Next", &n.next)) s.dirty = true;
  if (s.dlg.start != n.id && theme::SecondaryButton("Make start")) {
    s.dlg.start = n.id;
    s.dirty = true;
  }
  theme::SectionHeader("Choices", n.choices.empty() ? "none: continues to Next" : "shown when their If holds");
  int drop = -1;
  for (std::size_t i = 0; i < n.choices.size(); ++i) {
    story::DialogueChoice& c = n.choices[i];
    ImGui::PushID(static_cast<int>(i));
    ImGui::TextColored(theme::Copper(), "Choice %d", static_cast<int>(i + 1));
    if (input_str("Text", &c.text)) s.dirty = true;
    if (input_str("Next", &c.next)) s.dirty = true;
    if (input_str("If", &c.condition)) s.dirty = true;
    if (edit_lines("##choiceactions", &c.actions, 40)) s.dirty = true;
    if (ImGui::Checkbox("Once", &c.once)) s.dirty = true;
    ImGui::SameLine();
    if (theme::DangerButton("Remove")) drop = static_cast<int>(i);
    ImGui::Separator();
    ImGui::PopID();
  }
  if (drop >= 0) {
    n.choices.erase(n.choices.begin() + drop);
    s.dirty = true;
  }
  if (n.choices.size() < 4 && theme::SecondaryButton("Add choice")) {
    story::DialogueChoice c;
    c.text = "...";
    n.choices.push_back(c);
    s.dirty = true;
  }
  ImGui::SameLine();
  if (theme::SecondaryButton("Add linked node")) {
    story::DialogueNode m;
    m.id = unique_node_id(s.dlg);
    m.speaker = n.speaker;
    m.text = "...";
    m.x = n.x + kNodeW + 60;
    m.y = n.y;
    if (n.choices.empty()) n.next = m.id;
    else n.choices.back().next = m.id;
    s.dlg.nodes.push_back(m);
    s.dirty = true;
  }
  ImGui::SameLine();
  if (theme::DangerButton("Delete node")) {
    const std::string gone = n.id;
    s.dlg.nodes.erase(s.dlg.nodes.begin() + s.selected);
    for (story::DialogueNode& m : s.dlg.nodes) {
      if (m.next == gone) m.next.clear();
      for (story::DialogueChoice& c : m.choices) {
        if (c.next == gone) c.next.clear();
      }
    }
    s.selected = -1;
    s.dirty = true;
  }
  ImGui::EndChild();
}

void preview_section(DialoguePanelState& s) {
  if (!s.preview) return;
  story::DialogueRunner& r = s.preview_runner;
  theme::SectionHeader("Preview", "talks through the open file with scratch variables");
  if (!r.active()) {
    theme::StatusInfo("The talk ended.");
    return;
  }
  const story::DialogueRunner::Line& l = r.line();
  ImGui::TextColored(theme::Accent(), "%s", l.speaker.c_str());
  ImGui::TextWrapped("%s", l.text.c_str());
  for (std::size_t i = 0; i < l.choices.size(); ++i) {
    if (theme::SecondaryButton((std::to_string(i + 1) + ". " + l.choices[i] + "##pv" + std::to_string(i)).c_str())) {
      r.choose(static_cast<int>(i));
      break;
    }
  }
  if (l.choices.empty() && theme::PrimaryButton("Continue##pv")) r.advance();
}

void start_preview(DialoguePanelState& s) {
  s.preview = true;
  s.preview_vars.clear();
  s.preview_runner = story::DialogueRunner{};
  s.preview_runner.add(s.dlg);
  s.preview_runner.vars = &s.preview_vars;
  s.preview_runner.env.vars = &s.preview_vars;
  s.preview_runner.env.quest_status = [](const std::string&) { return std::string("inactive"); };
  s.preview_runner.run_action = [&s](const std::string& a) {
    story::Action x;
    if (!story::parse_action(a, &x, nullptr)) return;
    if (x.verb == "set") s.preview_vars.set(x.args[0], story::VarValue::parse(x.args[1]));
    else if (x.verb == "add") s.preview_vars.add(x.args[0], story::VarValue::parse(x.args[1]).num());
    else if (x.verb == "end") s.preview_runner.stop();
  };
  const std::string at = s.selected >= 0 && s.selected < static_cast<int>(s.dlg.nodes.size())
                             ? s.dlg.nodes[static_cast<std::size_t>(s.selected)].id
                             : std::string();
  s.preview_runner.start(s.dlg.id, at);
}

}  // namespace

// ---- Parley ------------------------------------------------------------------------

bool open_dialogue_file(DialoguePanelState& s, const std::string& rel) {
  story::Dialogue d;
  std::string err;
  if (!story::parse_dialogue(slurp(fs::path(s.project_dir) / rel), rel, &d, &err)) {
    s.status = err;
    return false;
  }
  // Nodes without a place get a grid.
  bool placed = false;
  for (const auto& n : d.nodes) placed = placed || n.x != 0 || n.y != 0;
  if (!placed) {
    for (std::size_t i = 0; i < d.nodes.size(); ++i) {
      d.nodes[i].x = static_cast<float>(i % 3) * (kNodeW + 50);
      d.nodes[i].y = static_cast<float>(i / 3) * 150.0f;
    }
  }
  s.dlg = std::move(d);
  s.path = rel;
  s.dirty = false;
  s.selected = -1;
  s.preview = false;
  s.lint = story::lint_dialogue(s.dlg);
  s.status = "Opened " + rel;
  return true;
}

bool save_dialogue_file(DialoguePanelState& s) {
  if (s.path.empty()) return false;
  if (!write_file(fs::path(s.project_dir) / s.path, json::write(story::dialogue_to_json(s.dlg)))) {
    s.status = "Could not write " + s.path;
    return false;
  }
  s.dirty = false;
  s.status = "Saved " + s.path;
  return true;
}

void draw_dialogue_panel(DialoguePanelState& s) {
  if (s.project_dir.empty()) s.project_dir = project_dir_now();
  const double now = ImGui::GetTime();
  if (s.scanned < 0 || now - s.scanned > 2.0) {
    s.files = scan_ext(s.project_dir, "dialogue", ".dlg.json");
    s.scanned = now;
  }
  ImGui::SetNextItemWidth(260);
  if (ImGui::BeginCombo("##dlgfile", s.path.empty() ? "Open a conversation..." : s.path.c_str())) {
    for (const std::string& f : s.files) {
      if (ImGui::Selectable(f.c_str(), f == s.path)) open_dialogue_file(s, f);
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(130);
  input_str("##dlgnew", &s.new_name);
  ImGui::SameLine();
  if (theme::SecondaryButton("New")) {
    story::Dialogue d;
    d.id = s.new_name;
    d.start = "hello";
    d.speakers["npc"] = story::Speaker{"Stranger", ""};
    story::DialogueNode n;
    n.id = "hello";
    n.speaker = "npc";
    n.text = "Howdy, {player}.";
    d.nodes.push_back(n);
    s.dlg = d;
    s.path = "dialogue/" + s.new_name + ".dlg.json";
    s.dirty = true;
    s.selected = 0;
    s.lint = story::lint_dialogue(s.dlg);
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!s.dirty || s.path.empty());
  if (theme::PrimaryButton("Save")) save_dialogue_file(s);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(s.path.empty());
  if (theme::SecondaryButton("Add node")) {
    story::DialogueNode n;
    n.id = unique_node_id(s.dlg);
    n.text = "...";
    n.x = -s.pan[0] + 60;
    n.y = -s.pan[1] + 60;
    s.dlg.nodes.push_back(n);
    s.selected = static_cast<int>(s.dlg.nodes.size()) - 1;
    s.dirty = true;
  }
  ImGui::SameLine();
  if (theme::CopperButton(s.preview ? "Restart preview" : "Preview")) start_preview(s);
  ImGui::EndDisabled();
  if (!s.status.empty()) ImGui::TextColored(theme::TextMuted(), "%s%s", s.status.c_str(), s.dirty ? "  (unsaved)" : "");

  std::string live_node;
  if (story::StoryRuntime* r = live()) {
    if (r->dialogue.active()) {
      if (r->dialogue.dialogue_id() == s.dlg.id) live_node = r->dialogue.line().node;
      ImGui::TextColored(theme::Success(), "Live: %s / %s  -  %s", r->dialogue.dialogue_id().c_str(),
                         r->dialogue.line().node.c_str(), clip(r->dialogue.line().text, 60).c_str());
    } else {
      ImGui::TextColored(theme::TextMuted(), "Riding: no conversation open (%llu lines shown)",
                         static_cast<unsigned long long>(r->dialogue.lines_shown()));
    }
  }
  if (s.path.empty()) {
    theme::StatusInfo("Pick a dialogue/*.dlg.json or make a new one.");
    return;
  }
  if (s.dirty) s.lint = story::lint_dialogue(s.dlg);
  const float lint_h = s.lint.empty() ? 0.0f : std::min(5.0f, static_cast<float>(s.lint.size())) * 18.0f + 6.0f;
  const float preview_h = s.preview ? 150.0f : 0.0f;
  ImGui::BeginChild("##dlg_main", ImVec2(0, -(lint_h + preview_h)));
  draw_graph(s, live_node);
  ImGui::SameLine();
  node_inspector(s);
  ImGui::EndChild();
  for (const std::string& l : s.lint) ImGui::TextColored(theme::Warning(), "%s", l.c_str());
  preview_section(s);
}

// ---- Bounty Board ------------------------------------------------------------------

namespace {
// Quest ids from the project's other files (links across files are fine).
std::vector<std::string> other_quest_ids(const QuestPanelState& s) {
  std::vector<std::string> ids;
  for (const std::string& f : scan_ext(s.project_dir, "quests", ".quest.json")) {
    if (f == s.path) continue;
    std::vector<story::QuestDef> qs;
    if (story::parse_quests(slurp(fs::path(s.project_dir) / f), f, &qs, nullptr)) {
      for (const auto& q : qs) ids.push_back(q.id);
    }
  }
  return ids;
}
}  // namespace

bool open_quest_file(QuestPanelState& s, const std::string& rel) {
  std::vector<story::QuestDef> qs;
  std::string err;
  if (!story::parse_quests(slurp(fs::path(s.project_dir) / rel), rel, &qs, &err)) {
    s.status = err;
    return false;
  }
  s.quests = std::move(qs);
  s.path = rel;
  s.selected = 0;
  s.dirty = false;
  s.lint = story::lint_quests(s.quests, other_quest_ids(s));
  s.status = "Opened " + rel;
  return true;
}

bool save_quest_file(QuestPanelState& s) {
  if (s.path.empty()) return false;
  json::Value v;
  if (s.quests.size() == 1) {
    v = story::quest_to_json(s.quests.front());
  } else {
    v = json::Value::object();
    json::Value a = json::Value::array();
    for (const auto& q : s.quests) a.push(story::quest_to_json(q));
    v.set("quests", a);
  }
  if (!write_file(fs::path(s.project_dir) / s.path, json::write(v))) {
    s.status = "Could not write " + s.path;
    return false;
  }
  s.dirty = false;
  s.status = "Saved " + s.path;
  return true;
}

void draw_quest_panel(QuestPanelState& s) {
  if (s.project_dir.empty()) s.project_dir = project_dir_now();
  const double now = ImGui::GetTime();
  if (s.scanned < 0 || now - s.scanned > 2.0) {
    s.files = scan_ext(s.project_dir, "quests", ".quest.json");
    s.scanned = now;
  }
  ImGui::SetNextItemWidth(260);
  if (ImGui::BeginCombo("##questfile", s.path.empty() ? "Open quests..." : s.path.c_str())) {
    for (const std::string& f : s.files) {
      if (ImGui::Selectable(f.c_str(), f == s.path)) open_quest_file(s, f);
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(130);
  input_str("##questnew", &s.new_name);
  ImGui::SameLine();
  if (theme::SecondaryButton("New")) {
    story::QuestDef q;
    q.id = s.new_name;
    q.title = "A New Bounty";
    q.objectives.push_back({"first", "Do the first thing", 1, false, false});
    s.quests = {q};
    s.path = "quests/" + s.new_name + ".quest.json";
    s.selected = 0;
    s.dirty = true;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!s.dirty || s.path.empty());
  if (theme::PrimaryButton("Save")) save_quest_file(s);
  ImGui::EndDisabled();
  if (!s.status.empty()) ImGui::TextColored(theme::TextMuted(), "%s%s", s.status.c_str(), s.dirty ? "  (unsaved)" : "");

  story::StoryRuntime* r = live();
  if (r && r->features().quests) {
    theme::SectionHeader("Riding", "live quest log");
    for (const story::QuestDef* d : r->quests.defs()) {
      ImGui::PushID(d->id.c_str());
      const story::QuestStatus st = r->quests.status(d->id);
      const ImVec4 col = st == story::QuestStatus::Active      ? theme::Accent()
                         : st == story::QuestStatus::Completed ? theme::Success()
                         : st == story::QuestStatus::Failed    ? theme::Danger()
                                                               : theme::TextMuted();
      ImGui::TextColored(col, "%-10s", story::quest_status_name(st));
      ImGui::SameLine();
      ImGui::Text("%s%s", d->title.c_str(), r->quests.tracked() == d->id ? "  (tracked)" : "");
      for (const story::ObjectiveDef& o : d->objectives) {
        ImGui::TextColored(theme::TextMuted(), "    %s %s %d/%d", r->quests.objective_done(d->id, o.id) ? "[x]" : "[ ]",
                           o.text.c_str(), r->quests.objective_progress(d->id, o.id), o.count);
      }
      if (st == story::QuestStatus::Inactive || st == story::QuestStatus::Failed) {
        if (theme::SecondaryButton("Start")) r->quests.start(d->id);
      } else if (st == story::QuestStatus::Active) {
        if (theme::SecondaryButton("Complete")) r->quests.complete(d->id);
        ImGui::SameLine();
        if (theme::DangerButton("Fail")) r->quests.fail(d->id);
      }
      r->quests_changed();
      ImGui::PopID();
    }
    if (theme::SecondaryButton(r->journal_open() ? "Close journal" : "Open journal")) r->show_journal(!r->journal_open());
  }
  if (s.path.empty()) {
    theme::StatusInfo("Pick a quests/*.quest.json or make a new one.");
    return;
  }
  theme::SectionHeader("Quests", s.path.c_str());
  ImGui::BeginChild("##qlist", ImVec2(180, 0), ImGuiChildFlags_Borders);
  for (std::size_t i = 0; i < s.quests.size(); ++i) {
    if (ImGui::Selectable((s.quests[i].title + "##q" + std::to_string(i)).c_str(), static_cast<int>(i) == s.selected)) {
      s.selected = static_cast<int>(i);
    }
  }
  if (theme::SecondaryButton("Add quest")) {
    story::QuestDef q;
    q.id = "quest" + std::to_string(s.quests.size() + 1);
    q.title = "Another Bounty";
    q.objectives.push_back({"first", "Do the first thing", 1, false, false});
    s.quests.push_back(q);
    s.selected = static_cast<int>(s.quests.size()) - 1;
    s.dirty = true;
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("##qedit", ImVec2(0, 0), ImGuiChildFlags_Borders);
  if (s.selected >= 0 && s.selected < static_cast<int>(s.quests.size())) {
    story::QuestDef& q = s.quests[static_cast<std::size_t>(s.selected)];
    bool ch = false;
    ch |= input_str("Id", &q.id);
    ch |= input_str("Title", &q.title);
    ch |= input_str("Giver", &q.giver);
    ImGui::TextColored(theme::TextMuted(), "Description");
    ch |= input_str("##qdesc", &q.description, true, 60);
    ImGui::TextColored(theme::TextMuted(), "Requires (quest ids, one per line)");
    ch |= edit_lines("##qreq", &q.prereqs, 36);
    ImGui::TextColored(theme::TextMuted(), "Rewards (actions, one per line)");
    ch |= edit_lines("##qrew", &q.rewards, 48);
    ch |= input_str("Next quest", &q.next);
    ch |= ImGui::Checkbox("Completes itself", &q.auto_complete);
    theme::SectionHeader("Objectives", "id, text, count");
    int drop = -1;
    for (std::size_t i = 0; i < q.objectives.size(); ++i) {
      story::ObjectiveDef& o = q.objectives[i];
      ImGui::PushID(static_cast<int>(i));
      ImGui::SetNextItemWidth(90);
      ch |= input_str("##oid", &o.id);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(230);
      ch |= input_str("##otext", &o.text);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(70);
      ch |= ImGui::InputInt("##ocount", &o.count);
      ImGui::SameLine();
      ch |= ImGui::Checkbox("opt", &o.optional);
      ImGui::SameLine();
      ch |= ImGui::Checkbox("hid", &o.hidden);
      ImGui::SameLine();
      if (theme::DangerButton("x")) drop = static_cast<int>(i);
      ImGui::PopID();
    }
    if (drop >= 0) {
      q.objectives.erase(q.objectives.begin() + drop);
      ch = true;
    }
    if (theme::SecondaryButton("Add objective")) {
      q.objectives.push_back({"step" + std::to_string(q.objectives.size() + 1), "...", 1, false, false});
      ch = true;
    }
    if (ch) {
      s.dirty = true;
      s.lint = story::lint_quests(s.quests, other_quest_ids(s));
    }
    for (const std::string& l : s.lint) ImGui::TextColored(theme::Warning(), "%s", l.c_str());
  }
  ImGui::EndChild();
}

// ---- Strongbox -----------------------------------------------------------------------

void draw_saves_panel(SavesPanelState& s) {
  if (s.project_dir.empty()) s.project_dir = project_dir_now();
  story::StoryRuntime* r = live();
  story::SaveStore disk(s.project_dir);
  if (r && r->features().saves) {
    theme::SectionHeader("Riding", "save / load this ride");
    ImGui::SetNextItemWidth(120);
    input_str("Slot", &s.slot);
    ImGui::SameLine();
    std::string err;
    if (theme::PrimaryButton("Save")) {
      s.status = r->save(s.slot, "Editor", &err) ? "Saved to " + s.slot : err;
    }
    ImGui::SameLine();
    if (theme::SecondaryButton("Autosave now")) {
      std::string slot;
      s.status = r->autosave(&slot, &err) ? "Autosaved to " + slot : err;
    }
    ImGui::SameLine();
    if (theme::SecondaryButton(r->save_menu_open() ? "Close slot menu" : "Slot menu")) r->show_save_menu(!r->save_menu_open());
    ImGui::TextColored(theme::TextMuted(), "Playtime %.0f s, %llu autosave(s); autosave every %.0f s%s", r->playtime(),
                       static_cast<unsigned long long>(r->autosaves()), r->settings.autosave_seconds,
                       r->settings.autosave_on_quest ? " and on quest completion" : "");
    theme::SectionHeader("Story variables", "live; edits apply now");
    for (const auto& [k, v] : r->vars.all()) {
      std::string t = v.str();
      ImGui::SetNextItemWidth(160);
      if (input_str(k.c_str(), &t)) r->vars.set(k, story::VarValue::parse(t));
    }
    if (r->vars.all().empty()) theme::StatusInfo("No variables yet.");
  } else {
    theme::StatusInfo("Play to save and load; slots on disk below.");
  }
  if (!s.status.empty()) ImGui::TextColored(theme::TextMuted(), "%s", s.status.c_str());
  theme::SectionHeader("Slots", disk.slots_dir().c_str());
  const auto slots = disk.list();
  if (slots.empty()) theme::StatusInfo("No saves yet.");
  for (const story::SlotInfo& i : slots) {
    ImGui::PushID(i.slot.c_str());
    ImGui::TextColored(i.autosave ? theme::Copper() : theme::Accent(), "%-8s", i.slot.c_str());
    ImGui::SameLine();
    ImGui::Text("%s  %s  %.0f s", i.label.c_str(), i.time.c_str(), i.playtime);
    ImGui::SameLine();
    if (r && r->features().saves && theme::SecondaryButton("Load")) {
      std::string err;
      s.status = r->load(i.slot, &err) ? "Loaded " + i.slot : err;
    }
    ImGui::SameLine();
    if (theme::DangerButton("Delete")) disk.remove(i.slot);
    ImGui::PopID();
  }
}

// ---- Install ---------------------------------------------------------------------------

void install_story_panels() {
  plugins::StoryPlugin::set_editor_hook([](const std::string& feature, plugin::Registrar& reg) {
    if (feature == "dialogue") {
      auto st = std::make_shared<DialoguePanelState>();
      plugin::EditorPanel p;
      p.id = "story.parley";
      p.title = "Parley";
      p.menu_label = "Parley (Dialogue)";
      p.before_begin = [] { ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver); };
      p.draw = [st] { draw_dialogue_panel(*st); };
      reg.add_editor_panel(std::move(p));
    } else if (feature == "quests") {
      auto st = std::make_shared<QuestPanelState>();
      plugin::EditorPanel p;
      p.id = "story.bounty_board";
      p.title = "Bounty Board";
      p.menu_label = "Bounty Board (Quests)";
      p.before_begin = [] { ImGui::SetNextWindowSize(ImVec2(760, 600), ImGuiCond_FirstUseEver); };
      p.draw = [st] { draw_quest_panel(*st); };
      reg.add_editor_panel(std::move(p));
    } else if (feature == "saves") {
      auto st = std::make_shared<SavesPanelState>();
      plugin::EditorPanel p;
      p.id = "story.strongbox";
      p.title = "Strongbox";
      p.menu_label = "Strongbox (Saves)";
      p.before_begin = [] { ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver); };
      p.draw = [st] { draw_saves_panel(*st); };
      reg.add_editor_panel(std::move(p));
    }
  });
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
