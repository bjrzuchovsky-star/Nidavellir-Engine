// Scripts in the 3D editor: the Inspector's script stack (several scripts per
// entity, each with its own props, enable toggle, reorder, remove), Add
// Component (New Script... / existing scripts), the Scripts list in the
// Assets panel (drag onto an entity; open in VS Code / Visual Studio) and
// the attach / write API the smokes drive.
#include "editor/editor3d/Editor3DScreen.h"

#include "editor/ui/Theme.h"
#include "runtime/Script.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <string>
#include <utility>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

ScriptStackHost Editor3DScreen::script_host() {
  ScriptStackHost host;
  host.project_dir = project_.path;
  host.project_name = project_.name;
  host.scripts = &scripts_;
  host.info = [this](const std::string& rel) -> const ScriptInfo& { return script_infos_.get(project_.path, rel); };
  host.note = [this](const std::string& m) { note(m); };
  host.read_only = playing();
  return host;
}

bool Editor3DScreen::set_scripts(std::uint64_t id, std::vector<ScriptData> stack, const std::string& label) {
  const scene3d::Entity3D* e = scene_.find(id);
  if (!e || playing()) return false;
  if (stack == script_stack(*e)) return false;
  scene3d::Entity3D c = *e;
  set_script_stack(c, std::move(stack));
  return replace_entity(c, label + " " + c.name);
}

bool Editor3DScreen::attach_script(std::uint64_t id, const std::string& rel) {
  const scene3d::Entity3D* e = scene_.find(id);
  if (!e || playing() || !runtime::valid_script_path(rel)) return false;
  std::vector<ScriptData> stack = script_stack(*e);
  for (const ScriptData& s : stack) {
    if (s.path == rel) {
      note(e->name + " already runs " + rel, runtime::LogLevel::Warn);
      return false;
    }
  }
  if (stack.size() >= kMaxScriptsPerEntity) {
    note(e->name + " already carries " + std::to_string(kMaxScriptsPerEntity) + " scripts", runtime::LogLevel::Warn);
    return false;
  }
  ScriptData s;
  s.path = rel;
  stack.push_back(std::move(s));
  const std::string name = e->name;
  if (!set_scripts(id, std::move(stack), "Attach Script")) return false;
  note("Attached " + rel + " to " + name);
  return true;
}

bool Editor3DScreen::new_script(std::uint64_t id, const std::string& name, ScriptLang lang,
                                const std::string& template_id, std::string* rel_out) {
  const NewScriptResult r = create_script_file(project_.path, name, lang, template_id);
  note(r.message, r.ok ? runtime::LogLevel::Info : runtime::LogLevel::Warn);
  if (!r.ok) return false;
  scripts_ = list_script_files(project_.path);
  script_infos_.clear();
  scripts_selected_ = r.rel;
  if (rel_out) *rel_out = r.rel;
  return id == 0 || attach_script(id, r.rel);
}

void Editor3DScreen::draw_add_component(std::uint64_t id, std::string* attach_rel) {
  ScriptStackHost host = script_host();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  const float w = std::min(260.0f, ImGui::GetContentRegionAvail().x);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f));
  ImGui::BeginDisabled(playing());
  if (theme::PrimaryButton("Add Component", ImVec2(w, 0)) || add_component_open_) {
    if (add_component_open_) ImGui::SetScrollHereY(1.0f);  // opened from code: bring it into view
    ImGui::OpenPopup("##add_component");
    add_component_open_ = false;
  }
  const ImVec2 below(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.0f);
  const float menu_w = ImGui::GetItemRectSize().x;
  ImGui::EndDisabled();
  if (std::string dropped = accept_script_drop(); !dropped.empty()) *attach_rel = std::move(dropped);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(playing() ? "Stop the ride to add components"
                                : "New Script..., an existing script, or drop a script here");
  }
  ImGui::SetNextWindowPos(below, ImGuiCond_Appearing);
  ImGui::SetNextWindowSizeConstraints(ImVec2(menu_w, 0), ImVec2(FLT_MAX, FLT_MAX));
  if (ImGui::BeginPopup("##add_component")) {
    const scene3d::Entity3D* e = scene_.find(id);
    ImGui::TextDisabled("Add to %s", e ? e->name.c_str() : "?");
    ImGui::Separator();
    if (std::string picked = add_script_menu_.draw_items(host); !picked.empty()) *attach_rel = std::move(picked);
    ImGui::Separator();
    // Built-in components (the inspector adds them after its own edit).
    if (ImGui::MenuItem("Camera", nullptr, false, e && !e->camera)) *attach_rel = "@camera";
    if (ImGui::MenuItem("Follow", nullptr, false, e && !e->follow)) *attach_rel = "@follow";
    ImGui::TextDisabled("Other components: the + buttons above");
    ImGui::EndPopup();
  }
  if (std::string created = add_script_menu_.draw_modal(host); !created.empty()) {
    scripts_ = list_script_files(project_.path);
    script_infos_.clear();
    *attach_rel = std::move(created);
  }
}

void Editor3DScreen::draw_scripts_panel() {
  ScriptsPanelState st{new_script_name_, sizeof(new_script_name_), &new_script_lang_, &new_script_template_,
                       &scripts_selected_};
  ScriptsPanelHost host;
  host.stack = script_host();
  host.playing = playing();
  host.create = [this](const std::string& name, ScriptLang lang, const std::string& templ) {
    return new_script(0, name, lang, templ);
  };
  if (const scene3d::Entity3D* target = scene_.find(primary())) {
    host.target_name = target->name;
    const std::uint64_t tid = target->id;
    host.target_has = [this, tid](const std::string& rel) {
      const scene3d::Entity3D* t = scene_.find(tid);
      if (!t) return false;
      for (const ScriptData& s : script_stack(*t)) {
        if (s.path == rel) return true;
      }
      return false;
    };
    host.attach = [this, tid](const std::string& rel) { attach_script(tid, rel); };
  }
  draw_scripts_panel_body(st, host);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
