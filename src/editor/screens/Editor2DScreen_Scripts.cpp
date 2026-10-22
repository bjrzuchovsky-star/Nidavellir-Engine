// Scripts: the claim's script files in every language (Scripts panel:
// write one from a template, open it in VS Code / Visual Studio / the OS
// editor, drag it onto an entity), the Inspector's script components (a
// Unity-style stack: several per entity, each with its own props, enable
// toggle, reorder and remove; every change one undo step + autosave), Add
// Component and hot reload while playing.

#include "editor/screens/Editor2DScreen.h"

#include "editor/launch/OpenWithOs.h"
#include "editor/scripting/BrandBook.h"
#include "runtime/CSharpScript.h"
#include "editor/ui/Theme.h"
#include "runtime/Script.h"
#include "scene/SampleScripts.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {

constexpr double kScriptsRefreshSeconds = 1.0;
constexpr std::size_t kMaxListed = 2000;

}  // namespace

// --- Files ----------------------------------------------------------------------

void Editor2DScreen::refresh_scripts() {
  scripts_refresh_time_ = now_seconds();
  scripts_ = list_script_files(project_.path, kMaxListed);
}

bool Editor2DScreen::create_script(const std::string& name,
                                   const std::string& template_id,
                                   std::string* rel_out) {
  return new_script(0, name, ScriptLang::Lua, template_id, rel_out);
}

bool Editor2DScreen::new_script(std::uint64_t id, const std::string& name, ScriptLang lang,
                                const std::string& template_id, std::string* rel_out) {
  const NewScriptResult r = create_script_file(project_.path, name, lang, template_id);
  note(r.message);
  if (!r.ok) return false;
  refresh_scripts();
  scripts_selected_ = r.rel;
  if (rel_out) *rel_out = r.rel;
  if (id != 0 && !attach_script(id, r.rel)) return false;
  return true;
}

bool Editor2DScreen::open_script(const std::string& rel) {
  std::string msg;
  const bool ok = open_script_external(ExternalEditor::Auto, project_.path, project_.name, rel, &msg);
  note(msg);
  return ok;
}

const ScriptInfo& Editor2DScreen::script_info(const std::string& rel) {
  ScriptInfo& info = script_infos_[rel];
  describe_script_into(script_lib_, project_.path, rel, &info);
  return info;
}

int Editor2DScreen::reload_play_scripts() {
  if (!is_playing()) {
    return 0;
  }
  const int n = play_.world().reload_scripts();
  if (n > 0) {
    note("Hot-reloaded " + std::to_string(n) + (n == 1 ? " script" : " scripts") +
         " mid-ride");
  }
  return n;
}

// --- Component edits ------------------------------------------------------------

bool Editor2DScreen::set_script(std::uint64_t id,
                                std::optional<ScriptData> script,
                                const std::string& label) {
  if (is_playing() || !workspace_.find(id)) {
    return false;
  }
  Workspace2D::Snapshot before = prepare_edit();
  Entity2D* e = workspace_.find(id);
  const std::optional<ScriptData> old = e->script;
  e->script = std::move(script);
  normalize_components(*e);
  if (e->script == old) {
    return false;
  }
  return commit_discrete(std::move(before), label, "");
}

bool Editor2DScreen::set_script_prop(std::uint64_t id, const std::string& name,
                                     std::optional<ScriptValue> value) {
  const Entity2D* e = workspace_.find(id);
  if (is_playing() || !e || !e->script || name.empty()) {
    return false;
  }
  ScriptData next = *e->script;
  if (value) {
    next.set(name, *value);
  } else if (!next.erase(name)) {
    return false;
  }
  return set_script(id, std::move(next),
                    value ? "Script Prop: " + name : "Reset Prop: " + name);
}

bool Editor2DScreen::set_scripts(std::uint64_t id, std::vector<ScriptData> stack,
                                 const std::string& label) {
  const Entity2D* cur = workspace_.find(id);
  if (is_playing() || !cur || cur->tilemap) {
    return false;
  }
  if (stack == script_stack(*cur)) {
    return false;
  }
  Workspace2D::Snapshot before = prepare_edit();
  Entity2D* e = workspace_.find(id);
  set_script_stack(*e, std::move(stack));
  normalize_components(*e);
  return commit_discrete(std::move(before), label, "");
}

bool Editor2DScreen::attach_script(std::uint64_t id, const std::string& rel) {
  const Entity2D* e = workspace_.find(id);
  if (is_playing() || !e || e->tilemap || !runtime::valid_script_path(rel)) {
    return false;
  }
  std::vector<ScriptData> stack = script_stack(*e);
  for (const ScriptData& s : stack) {
    if (s.path == rel) {
      note(e->name + " already runs " + rel);
      return false;
    }
  }
  if (stack.size() >= kMaxScriptsPerEntity) {
    note(e->name + " already carries " + std::to_string(kMaxScriptsPerEntity) + " scripts");
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

// --- Inspector ------------------------------------------------------------------

void Editor2DScreen::draw_inspector_script(std::uint64_t id) {
  const Entity2D* e = workspace_.find(id);
  if (!e || e->tilemap) {
    return;
  }
  if (now_seconds() - scripts_refresh_time_ > kScriptsRefreshSeconds) {
    refresh_scripts();
  }
  ImGui::PushID("scripts");
  ScriptStackHost host;
  host.project_dir = project_.path;
  host.project_name = project_.name;
  host.scripts = &scripts_;
  host.info = [this](const std::string& rel) -> const ScriptInfo& { return script_info(rel); };
  host.after_item = [this](const char* label) { track_inspector_item(label); };
  host.note = [this](const std::string& m) { note(m); };
  host.read_only = is_playing();
  std::vector<ScriptData> stack = script_stack(*e);
  ImGui::BeginGroup();
  if (!stack.empty()) {
    ImGui::TextColored(theme::Accent(), stack.size() == 1 ? "Script" : "Scripts (run top to bottom)");
  }
  const StackResult r = draw_script_stack(stack, host);
  ImGui::EndGroup();
  if (const std::string dropped = accept_script_drop(); !dropped.empty()) {
    attach_script(id, dropped);
  } else if (r.change == StackChange::Commit) {
    set_scripts(id, std::move(stack), r.label);
  } else if (r.change == StackChange::Live) {
    if (Entity2D* t = workspace_.find(id)) {
      set_script_stack(*t, std::move(stack));
      mark_dirty();
    }
  }
  ImGui::PopID();
}

void Editor2DScreen::draw_inspector_add_component(std::uint64_t id) {
  const Entity2D* e = workspace_.find(id);
  if (!e) {
    return;
  }
  ScriptStackHost host;
  host.project_dir = project_.path;
  host.project_name = project_.name;
  host.scripts = &scripts_;
  host.note = [this](const std::string& m) { note(m); };
  host.read_only = is_playing();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  const float w = std::min(260.0f, ImGui::GetContentRegionAvail().x);
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f));
  ImGui::BeginDisabled(is_playing());
  if (theme::PrimaryButton("Add Component", ImVec2(w, 0)) || add_component_open_) {
    if (add_component_open_) ImGui::SetScrollHereY(1.0f);  // opened from code: bring it into view
    ImGui::OpenPopup("##add_component");
    add_component_open_ = false;
  }
  const ImVec2 below(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.0f);
  const float menu_w = ImGui::GetItemRectSize().x;
  ImGui::EndDisabled();
  if (!e->tilemap) {
    if (const std::string dropped = accept_script_drop(); !dropped.empty()) attach_script(id, dropped);
  }
  e = workspace_.find(id);
  if (!e) return;
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(is_playing() ? "Stop the ride to add components"
                                   : "Colliders, riders, cameras, scripts... (or drop a script here)");
  }
  ImGui::SetNextWindowPos(below, ImGuiCond_Appearing);
  ImGui::SetNextWindowSizeConstraints(ImVec2(menu_w, 0), ImVec2(FLT_MAX, FLT_MAX));
  if (ImGui::BeginPopup("##add_component")) {
    ImGui::TextDisabled("Add to %s", e->name.c_str());
    ImGui::Separator();
    if (!e->tilemap) {
      const std::string picked = add_script_menu_.draw_items(host);
      if (!picked.empty()) attach_script(id, picked);
      ImGui::Separator();
    }
    e = workspace_.find(id);
    if (ImGui::MenuItem("Collider", nullptr, false, e && !e->collider)) add_component(id, "Collider");
    if (ImGui::MenuItem("Player", nullptr, false, e && !e->player)) add_component(id, "Player");
    if (ImGui::MenuItem("Camera", nullptr, false, e && !e->camera)) add_component(id, "Camera");
    if (ImGui::MenuItem("Spawn Point", nullptr, false, e && !e->spawn)) add_component(id, "Spawn");
    ImGui::EndPopup();
  }
  if (const std::string created = add_script_menu_.draw_modal(host); !created.empty()) {
    refresh_scripts();
    attach_script(id, created);
  }
}

// --- Scripts panel --------------------------------------------------------------

void Editor2DScreen::draw_scripts_panel() {
  if (now_seconds() - scripts_refresh_time_ > kScriptsRefreshSeconds) {
    refresh_scripts();
  }
  const bool playing = is_playing();
  ScriptsPanelState st{new_script_name_, sizeof(new_script_name_), &new_script_lang_, &new_script_template_,
                       &scripts_selected_};
  ScriptsPanelHost host;
  host.stack.project_dir = project_.path;
  host.stack.project_name = project_.name;
  host.stack.scripts = &scripts_;
  host.stack.info = [this](const std::string& rel) -> const ScriptInfo& { return script_info(rel); };
  host.stack.note = [this](const std::string& m) { note(m); };
  host.stack.read_only = playing;
  host.playing = playing;
  host.create = [this](const std::string& name, ScriptLang lang, const std::string& templ) {
    std::string rel;
    return new_script(0, name, lang, templ, &rel);
  };
  const Entity2D* target = workspace_.selected();
  if (target && !target->tilemap) {
    host.target_name = target->name;
    host.target_has = [target](const std::string& rel) {
      for (const ScriptData& s : script_stack(*target)) {
        if (s.path == rel) return true;
      }
      return false;
    };
    const std::uint64_t tid = target->id;
    host.attach = [this, tid](const std::string& rel) { attach_script(tid, rel); };
  }
  draw_scripts_panel_body(st, host);
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
