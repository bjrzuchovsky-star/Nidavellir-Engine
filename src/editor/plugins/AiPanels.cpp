#include "editor/plugins/AiPanels.h"
#include "editor/ux/Tooltip.h"

#include "ai/BehaviorTree.h"
#include "ai3d/AiWorld3D.h"
#include "editor/ui/Theme.h"
#include "plugins/ai/AiPlugin.h"
#include "runtime/World.h"

#include <imgui.h>

#include <algorithm>
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

ImVec4 kind_colour(ai::BtKind k) {
  switch (k) {
    case ai::BtKind::Composite: return theme::Accent();
    case ai::BtKind::Decorator: return theme::Copper();
    case ai::BtKind::Condition: return theme::Sand();
    default: return theme::Text();
  }
}

ImVec4 status_colour(ai::BtStatus s) {
  switch (s) {
    case ai::BtStatus::Running: return theme::Success();
    case ai::BtStatus::Success: return ImVec4(0.45f, 0.65f, 1.0f, 1.0f);
    case ai::BtStatus::Failure: return theme::Danger();
    default: return theme::TextMuted();
  }
}

const char* status_name(ai::BtStatus s) {
  switch (s) {
    case ai::BtStatus::Running: return "running";
    case ai::BtStatus::Success: return "success";
    case ai::BtStatus::Failure: return "failure";
    default: return "idle";
  }
}

void scan(const std::string& project, BtPanelState& s) {
  s.files.clear();
  std::error_code ec;
  if (project.empty()) return;
  for (auto it = fs::recursive_directory_iterator(project, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (it->is_directory(ec) && (name == ".tombstone" || name == "saves" || name == "export")) {
      it.disable_recursion_pending();
      continue;
    }
    if (name.size() > 8 && name.compare(name.size() - 8, 8, ".bt.json") == 0) {
      s.files.push_back(fs::relative(it->path(), project, ec).generic_string());
    }
  }
  std::sort(s.files.begin(), s.files.end());
}

bool open_tree(const std::string& project, const std::string& rel, BtPanelState& s) {
  std::ifstream f(fs::path(project) / rel, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  ai::BtTree t;
  std::string err;
  if (!f || !t.parse(ss.str(), &err)) {
    s.status = "Could not read " + rel + ": " + err;
    return false;
  }
  s.tree = std::move(t);
  s.path = rel;
  s.dirty = false;
  s.selected = 0;
  s.lint = s.tree.lint();
  s.status = "Opened " + rel;
  return true;
}

bool save_tree(const std::string& project, BtPanelState& s) {
  std::error_code ec;
  const fs::path p = fs::path(project) / s.path;
  fs::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  f << s.tree.write();
  if (!f) {
    s.status = "Could not write " + s.path;
    return false;
  }
  s.dirty = false;
  s.lint = s.tree.lint();
  s.status = "Saved " + s.path + " (agents pick it up on the next Play)";
  return true;
}

// Node row (recursive): kind colour, label, live status when watching.
void node_rows(BtPanelState& s, int i, int depth, const ai::BtInstance* live) {
  const ai::BtNode& n = s.tree.nodes[static_cast<std::size_t>(i)];
  const ai::BtNodeInfo* info = ai::bt_node_info(n.type);
  ImGui::PushID(i);
  ImGui::Indent(depth == 0 ? 0.0f : 14.0f);
  const bool sel = s.selected == i;
  char buf[160];
  std::snprintf(buf, sizeof buf, "%s%s", n.label().c_str(),
                n.name.empty() || n.name == n.type ? "" : (" (" + n.type + ")").c_str());
  ImGui::PushStyleColor(ImGuiCol_Text, info ? kind_colour(info->kind) : theme::Warning());
  if (ImGui::Selectable(buf, sel, 0, ImVec2(ImGui::GetContentRegionAvail().x - 90.0f, 0))) s.selected = i;
  ImGui::PopStyleColor();
  if (info && ImGui::IsItemHovered()) ux::SetTooltip("%s", info->help);
  if (live && i < static_cast<int>(live->tree().nodes.size())) {
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 80.0f);
    const ai::BtStatus st = live->node_status(i);
    const bool fresh = live->node_tick(i) + 1 >= live->ticks();
    ImVec4 c = status_colour(st);
    if (!fresh) c.w *= 0.4f;
    ImGui::TextColored(c, "%s", status_name(st));
  }
  for (int c : n.children) node_rows(s, c, depth + 1, live);
  ImGui::Unindent(depth == 0 ? 0.0f : 14.0f);
  ImGui::PopID();
}

void param_editor(BtPanelState& s) {
  if (s.selected < 0 || s.selected >= static_cast<int>(s.tree.nodes.size())) return;
  ai::BtNode& n = s.tree.nodes[static_cast<std::size_t>(s.selected)];
  const ai::BtNodeInfo* info = ai::bt_node_info(n.type);
  ImGui::TextColored(theme::Accent(), "%s", n.type.c_str());
  if (info) {
    ImGui::SameLine();
    ImGui::TextColored(theme::TextMuted(), "- %s", info->help);
  }
  char name[96];
  std::snprintf(name, sizeof name, "%s", n.name.c_str());
  if (ImGui::InputText("Name", name, sizeof name)) {
    n.name = name;
    s.dirty = true;
  }
  // Every hinted parameter plus whatever the file already carries.
  std::vector<std::string> keys;
  if (info) {
    std::istringstream hs(info->params);
    std::string k;
    while (hs >> k) keys.push_back(k);
  }
  if (n.type == "sequence" || n.type == "selector") keys.push_back("reactive");
  for (const auto& [k, v] : n.params.members()) {
    if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
  }
  for (const std::string& k : keys) {
    const ai::JsonValue& v = n.params.get(k);
    std::string text = v.is_null() ? "" : v.is_string() ? v.str() : v.dump(0);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%s", text.c_str());
    if (ImGui::InputText(k.c_str(), buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue) ||
        (ImGui::IsItemDeactivatedAfterEdit())) {
      ai::JsonValue parsed;
      std::string err;
      if (buf[0] == '\0') {
        parsed = ai::JsonValue();
      } else if (!ai::JsonValue::parse(buf, &parsed, &err)) {
        parsed = ai::JsonValue::string(buf);
      }
      n.params.set(k, parsed);
      s.dirty = true;
    }
  }
}

void live_section(plugins::AiPlugin& p, BtPanelState& s) {
  // Agents riding now: 2D (the plugin's world) or 3D (the editor's World3D).
  struct Row {
    std::uint64_t id;
    std::string label;
    const ai::AgentBrain* brain;
  };
  std::vector<Row> rows;
  if (runtime::World* w = p.current_world()) {
    for (const plugins::AiPlugin::Agent* a : p.agents(*w)) {
      const runtime::Actor* act = w->find(a->id);
      rows.push_back({a->id, act ? act->data.name : std::to_string(a->id), a->brain.get()});
    }
  }
  if (ai3d::AiWorld3D* a3 = ai3d::AiWorld3D::latest()) {
    for (std::uint64_t id : a3->agent_ids()) {
      const auto* e = a3->world()->find(id);
      rows.push_back({id, e ? e->name : std::to_string(id), a3->brain(id)});
    }
  }
  bool dbg = p.debug();
  if (ai3d::AiWorld3D* a3 = ai3d::AiWorld3D::latest()) dbg = dbg || a3->debug();
  if (ImGui::Checkbox("Debug overlay (paths, navmesh, BT state, sight)", &dbg)) {
    p.set_debug(dbg);
    if (ai3d::AiWorld3D* a3 = ai3d::AiWorld3D::latest()) a3->set_debug(dbg);
  }
  if (rows.empty()) {
    ImGui::TextColored(theme::TextMuted(), "No agents riding. Give an entity an agent (ai.agent in Lua / an agent component) and Play to watch its tree live.");
    return;
  }
  ImGui::SetNextItemWidth(220.0f);
  std::string current = "(pick an agent)";
  for (const Row& r : rows) {
    if (r.id == s.watch) current = r.label;
  }
  if (ImGui::BeginCombo("Watch", current.c_str())) {
    for (const Row& r : rows) {
      if (ImGui::Selectable(r.label.c_str(), r.id == s.watch)) s.watch = r.id;
    }
    ImGui::EndCombo();
  }
  const ai::AgentBrain* brain = nullptr;
  for (const Row& r : rows) {
    if (r.id == s.watch) brain = r.brain;
  }
  if (!brain) return;
  if (const ai::BtInstance* bt = brain->tree()) {
    ImGui::TextColored(status_colour(bt->last_status()), "%s", bt->running_path().c_str());
    ImGui::TextColored(theme::TextMuted(), "%llu ticks, %.1f s", static_cast<unsigned long long>(bt->ticks()),
                       bt->time());
  }
  if (ImGui::TreeNodeEx("Blackboard", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (const auto& [k, v] : brain->blackboard().values()) {
      ImGui::TextColored(theme::Sand(), "%s", k.c_str());
      ImGui::SameLine(140.0f);
      ImGui::TextUnformatted(v.str().c_str());
    }
    ImGui::TreePop();
  }
  if (ImGui::TreeNodeEx("Senses")) {
    for (const ai::Percept& pc : brain->perception().known()) {
      ImGui::Text("#%llu %s%s", static_cast<unsigned long long>(pc.id), pc.visible ? "seen " : "",
                  pc.sound.empty() ? "" : ("heard " + pc.sound).c_str());
    }
    ImGui::TreePop();
  }
}

}  // namespace

void draw_bt_panel(plugins::AiPlugin& p, BtPanelState& s) {
  const std::string& project = p.project_dir();
  const double now = ImGui::GetTime();
  if (s.scanned < 0.0 || now - s.scanned > 3.0) {
    scan(project, s);
    s.scanned = now;
  }
  // Toolbar: open / new / save.
  ImGui::SetNextItemWidth(220.0f);
  if (ImGui::BeginCombo("##bt_file", s.path.empty() ? "Open a tree..." : s.path.c_str())) {
    for (const std::string& f : s.files) {
      if (ImGui::Selectable(f.c_str(), f == s.path)) open_tree(project, f, s);
    }
    if (s.files.empty()) ImGui::TextColored(theme::TextMuted(), "No *.bt.json in the project yet");
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  char nb[64];
  std::snprintf(nb, sizeof nb, "%s", s.new_name.c_str());
  ImGui::SetNextItemWidth(110.0f);
  if (ImGui::InputText("##bt_new", nb, sizeof nb)) s.new_name = nb;
  ImGui::SameLine();
  if (ImGui::Button("New")) {
    s.tree = ai::BtTree{};
    s.tree.name = s.new_name;
    s.tree.add(-1, "selector");
    s.path = "ai/" + s.new_name + ".bt.json";
    s.selected = 0;
    save_tree(project, s);
    s.scanned = -1.0;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(s.path.empty() || !s.dirty);
  if (ImGui::Button(s.dirty ? "Save*" : "Save")) save_tree(project, s);
  ImGui::EndDisabled();
  if (!s.status.empty()) ImGui::TextColored(theme::TextMuted(), "%s", s.status.c_str());
  ImGui::Separator();

  const ai::BtInstance* live = nullptr;
  // Live colours when the watched agent runs this very file.
  if (runtime::World* w = p.current_world()) {
    if (plugins::AiPlugin::Agent* a = p.agent(*w, s.watch)) {
      if (a->bt_path == s.path) live = a->brain->tree();
    }
  }
  if (!live) {
    if (ai3d::AiWorld3D* a3 = ai3d::AiWorld3D::latest()) {
      if (ai::AgentBrain* b = a3->brain(s.watch)) {
        if (b->tree() && b->tree()->tree().name == s.tree.name) live = b->tree();
      }
    }
  }
  if (live && live->tree().nodes.size() != s.tree.nodes.size()) live = nullptr;  // edited since

  if (!s.tree.empty()) {
    const float h = ImGui::GetContentRegionAvail().y * 0.5f;
    ImGui::BeginChild("bt_tree", ImVec2(0, std::max(160.0f, h)), true);
    node_rows(s, 0, 0, live);
    ImGui::EndChild();
    // Palette + structure edits on the selected node.
    const auto& types = ai::bt_node_types();
    s.palette = std::clamp(s.palette, 0, static_cast<int>(types.size()) - 1);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("##palette", types[static_cast<std::size_t>(s.palette)].type)) {
      for (int k = 0; k < static_cast<int>(types.size()); ++k) {
        ImGui::PushStyleColor(ImGuiCol_Text, kind_colour(types[static_cast<std::size_t>(k)].kind));
        if (ImGui::Selectable(types[static_cast<std::size_t>(k)].type, k == s.palette)) s.palette = k;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ux::SetTooltip("%s", types[static_cast<std::size_t>(k)].help);
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add child")) {
      const int c = s.tree.add(s.selected, types[static_cast<std::size_t>(s.palette)].type);
      if (c >= 0) s.selected = c, s.dirty = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s.selected <= 0);
    if (ImGui::Button("Up")) s.tree.move(s.selected, -1), s.dirty = true;
    ImGui::SameLine();
    if (ImGui::Button("Down")) s.tree.move(s.selected, +1), s.dirty = true;
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
      s.tree.remove(s.selected);
      s.selected = 0;
      s.dirty = true;
    }
    ImGui::EndDisabled();
    param_editor(s);
    if (s.dirty) s.lint = s.tree.lint();
    for (const std::string& l : s.lint) ImGui::TextColored(theme::Warning(), "%s", l.c_str());
  }
  ImGui::Separator();
  live_section(p, s);
}

void install_ai_panels() {
  plugins::AiPlugin::set_editor_hook([](plugins::AiPlugin& p, plugin::Registrar& reg) {
    plugins::AiPlugin* ap = &p;
    auto state = std::make_shared<BtPanelState>();
    plugin::EditorPanel bt;
    bt.id = "ai.behaviour_trees";
    bt.title = "Behaviour Trees";
    bt.menu_label = "Behaviour Trees (AI)";
    bt.before_begin = [] { ImGui::SetNextWindowSize(ImVec2(520, 620), ImGuiCond_FirstUseEver); };
    bt.draw = [ap, state] { draw_bt_panel(*ap, *state); };
    reg.add_editor_panel(std::move(bt));
  });
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
