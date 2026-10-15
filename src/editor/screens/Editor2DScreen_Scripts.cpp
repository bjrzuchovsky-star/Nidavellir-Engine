// Scripts: the project's Lua files (Scripts panel: write one from a
// template, open it in the OS editor, attach it), the Inspector's Script
// section (file + per-entity props, each change one undo step + autosave)
// and hot reload while playing.

#include "editor/screens/Editor2DScreen.h"

#include "editor/launch/OpenWithOs.h"
#include "editor/scripting/BrandBook.h"
#include "runtime/CSharpScript.h"
#include "editor/ui/Theme.h"
#include "runtime/Script.h"
#include "scene/SampleScripts.h"

#include <imgui.h>

#include <algorithm>
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

constexpr const char* kScriptsDir = "scripts";
constexpr double kScriptsRefreshSeconds = 1.0;
constexpr std::size_t kMaxListed = 2000;

// "Gate Keeper" -> "gate_keeper": letters, digits, '_' and '-'; spaces
// become '_', anything else is dropped. A trailing ".lua" is ignored.
std::string script_file_stem(std::string name) {
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".lua") == 0) {
    name.resize(name.size() - 4);
  }
  std::string out;
  for (const char ch : name) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || c == '_' || c == '-') {
      out.push_back(static_cast<char>(std::tolower(c)));
    } else if (c == ' ' && !out.empty() && out.back() != '_') {
      out.push_back('_');
    }
    if (out.size() >= 48) break;
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

}  // namespace

// --- Files ----------------------------------------------------------------------

void Editor2DScreen::refresh_scripts() {
  scripts_.clear();
  scripts_refresh_time_ = now_seconds();
  if (project_.path.empty()) {
    return;
  }
  const fs::path root(project_.path);
  std::error_code ec;
  if (!fs::is_directory(root / kScriptsDir, ec)) {
    return;
  }
  fs::recursive_directory_iterator it(
      root / kScriptsDir, fs::directory_options::skip_permission_denied, ec);
  for (; !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    std::error_code fec;
    if (!it->is_regular_file(fec)) {
      continue;
    }
    const std::string rel =
        fs::relative(it->path(), root, fec).generic_string();
    if (!fec && runtime::valid_script_path(rel)) {
      scripts_.push_back(rel);
      if (scripts_.size() >= kMaxListed) break;
    }
  }
  std::sort(scripts_.begin(), scripts_.end());
}

bool Editor2DScreen::create_script(const std::string& name,
                                   const std::string& template_id,
                                   std::string* rel_out) {
  const sample_scripts::Template* t = sample_scripts::find_template(template_id);
  if (!t) {
    note("No script template called " + template_id);
    return false;
  }
  if (project_.path.empty()) {
    note("No project folder to keep scripts in.");
    return false;
  }
  const std::string stem = script_file_stem(name);
  const std::string rel = std::string(kScriptsDir) + "/" + stem + ".lua";
  if (stem.empty() || !runtime::valid_script_path(rel)) {
    note("Give the script a name (letters, digits, _ or -).");
    return false;
  }
  const fs::path full = fs::path(project_.path) / rel;
  std::error_code ec;
  if (fs::exists(full, ec)) {
    note(rel + " already exists. Pick another name.");
    return false;
  }
  fs::create_directories(full.parent_path(), ec);
  {
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    out << t->source;
    if (!out) {
      note("Could not write " + rel);
      return false;
    }
  }
  refresh_scripts();
  scripts_selected_ = rel;
  if (rel_out) *rel_out = rel;
  note("Wrote " + rel + " from the " + t->label + " template");
  return true;
}

bool Editor2DScreen::open_script(const std::string& rel) {
  if (project_.path.empty() || !runtime::valid_script_path(rel)) {
    return false;
  }
  if (rel.size() > 11 && rel.compare(rel.size() - 11, 11, ".graph.json") == 0) {
    BrandBook::get().open_graph(project_.path, project_.name, rel);
    note("Opened " + rel + " in the Brand Book. Save it and the ride picks it up.");
    return true;
  }
  std::string err;
  const std::string full = (fs::path(project_.path) / rel).string();
  if (!launcher::open_with_os(full, &err)) {
    note("Could not open " + rel + ": " + err);
    return false;
  }
  note("Opened " + rel + " in your editor. Save it and the ride picks it up.");
  return true;
}

const ScriptInfo& Editor2DScreen::script_info(const std::string& rel) {
  ScriptInfo& info = script_infos_[rel];
  if (!runtime::valid_script_path(rel)) {
    info = ScriptInfo{};
    info.error = "scripts live under scripts/ and end in .lua";
    return info;
  }
  const runtime::ScriptLibrary::Entry* e = script_lib_.get(project_.path, rel);
  if (!e) {
    info = ScriptInfo{};
    info.error = "could not read " + rel;
    return info;
  }
  if (info.version != 0 && info.version == e->version) {
    return info;
  }
  const bool ok = e->ok;
  const std::string text = e->text;
  const std::string read_error = e->error;
  info = ScriptInfo{};
  info.version = e->version;
  if (!ok) {
    info.error = read_error;
    return info;
  }
  std::string err;
  info.ok = runtime::ScriptHost::describe(text, rel, &info.defaults, &err);
  info.error = err;
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

// --- Inspector ------------------------------------------------------------------

void Editor2DScreen::draw_inspector_script(std::uint64_t id) {
  const Entity2D* e = workspace_.find(id);
  if (!e || e->tilemap) {
    return;
  }
  if (now_seconds() - scripts_refresh_time_ > kScriptsRefreshSeconds) {
    refresh_scripts();
  }
  ImGui::PushID("script");
  ImGui::TextColored(theme::Accent(), "Script");
  if (!e->script) {
    if (scripts_.empty()) {
      ImGui::TextDisabled("No scripts yet. Write one in the Scripts panel.");
    } else if (ImGui::BeginCombo("Attach", "(no script)")) {
      for (const std::string& rel : scripts_) {
        if (ImGui::Selectable(rel.c_str())) {
          ScriptData s;
          s.path = rel;
          set_script(id, s, "Attach Script");
          break;  // entities() may have been replaced by the commit
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PopID();
    return;
  }

  const std::string path = e->script->path;
  if (ImGui::BeginCombo("File", path.c_str())) {
    for (const std::string& rel : scripts_) {
      if (ImGui::Selectable(rel.c_str(), rel == path) && rel != path) {
        ScriptData s = *e->script;
        s.path = rel;
        set_script(id, s, "Script File");
        break;
      }
    }
    ImGui::EndCombo();
  }
  if (theme::SecondaryButton("Open", ImVec2(64, 0))) {
    open_script(path);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Open %s in the OS default editor", path.c_str());
  }
  ImGui::SameLine();
  if (theme::DangerButton("Detach Script", ImVec2(120, 0))) {
    set_script(id, std::nullopt, "Detach Script");
    ImGui::PopID();
    return;
  }

  const ScriptInfo info = script_info(path);
  if (!info.ok) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
    ImGui::TextWrapped("%s", info.error.empty() ? "Script does not run"
                                                : info.error.c_str());
    ImGui::PopStyleColor();
  } else if (info.defaults.empty()) {
    ImGui::TextDisabled("No props. Declare `props = { ... }` in the file.");
  }

  for (const ScriptProp& d : info.defaults) {
    e = workspace_.find(id);
    if (!e || !e->script) {
      break;
    }
    const ScriptProp* o = e->script->find(d.name);
    const bool overridden = o && o->value.type == d.value.type;
    const ScriptValue& v = overridden ? o->value : d.value;
    ImGui::PushID(d.name.c_str());
    if (overridden) {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
    }
    switch (d.value.type) {
      case ScriptValue::Type::Bool: {
        bool b = v.flag;
        if (ImGui::Checkbox(d.name.c_str(), &b)) {
          set_script_prop(id, d.name, ScriptValue::of_bool(b));
        }
        break;
      }
      case ScriptValue::Type::Number: {
        double x = v.number;
        if (ImGui::DragScalar(d.name.c_str(), ImGuiDataType_Double, &x, 0.1f,
                              nullptr, nullptr, "%.6g")) {
          if (Entity2D* t = workspace_.find(id); t && t->script) {
            t->script->set(d.name, ScriptValue::of_number(x));
            mark_dirty();
          }
        }
        track_inspector_item("Script Prop");
        break;
      }
      case ScriptValue::Type::Text: {
        char buf[ScriptData::kMaxTextLength + 1];
        std::snprintf(buf, sizeof(buf), "%s", v.text.c_str());
        if (ImGui::InputText(d.name.c_str(), buf, sizeof(buf))) {
          if (Entity2D* t = workspace_.find(id); t && t->script) {
            t->script->set(d.name, ScriptValue::of_text(buf));
            mark_dirty();
          }
        }
        track_inspector_item("Script Prop");
        break;
      }
    }
    if (overridden) {
      ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::SmallButton("default")) {
        set_script_prop(id, d.name, std::nullopt);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Back to the file's %s", describe(d.value).c_str());
      }
    }
    ImGui::PopID();
  }

  // Overrides the file no longer declares (renamed / removed props).
  e = workspace_.find(id);
  if (e && e->script && info.ok) {
    std::vector<std::string> stale;
    for (const ScriptProp& p : e->script->props) {
      const bool declared = std::any_of(
          info.defaults.begin(), info.defaults.end(),
          [&p](const ScriptProp& d) { return d.name == p.name; });
      if (!declared) stale.push_back(p.name);
    }
    for (const std::string& name : stale) {
      ImGui::PushID(name.c_str());
      ImGui::TextDisabled("%s (not in the file any more)", name.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("drop")) {
        set_script_prop(id, name, std::nullopt);
      }
      ImGui::PopID();
    }
  }
  ImGui::PopID();
}

// --- Scripts panel --------------------------------------------------------------

void Editor2DScreen::draw_scripts_panel() {
  if (now_seconds() - scripts_refresh_time_ > kScriptsRefreshSeconds) {
    refresh_scripts();
  }
  const auto& templates = sample_scripts::templates();
  new_script_template_ = std::clamp(new_script_template_, 0,
                                    static_cast<int>(templates.size()) - 1);
  const bool playing = is_playing();

  theme::SectionHeader("Write a script", "scripts/<name>.lua");
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##new_script", "name, e.g. gate_keeper",
                           new_script_name_, sizeof(new_script_name_));
  ImGui::SetNextItemWidth(-90.0f);
  if (ImGui::BeginCombo("##template",
                        templates[static_cast<std::size_t>(new_script_template_)]
                            .label)) {
    for (int i = 0; i < static_cast<int>(templates.size()); ++i) {
      const sample_scripts::Template& t = templates[static_cast<std::size_t>(i)];
      if (ImGui::Selectable(t.label, i == new_script_template_)) {
        new_script_template_ = i;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", t.blurb);
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(new_script_name_[0] == '\0');
  if (theme::PrimaryButton("Write", ImVec2(82, 0))) {
    std::string rel;
    if (create_script(new_script_name_,
                      templates[static_cast<std::size_t>(new_script_template_)].id,
                      &rel)) {
      new_script_name_[0] = '\0';
    }
  }
  ImGui::EndDisabled();

  if (theme::SecondaryButton("Brand Book (visual scripts)", ImVec2(-1, 0))) {
    BrandBook::get().open(project_.path, project_.name);
  }
  theme::SectionHeader("Scripts", playing ? "saves hot-reload mid-ride"
                                          : "double-click to open");
  if (scripts_.empty()) {
    ImGui::TextDisabled("No scripts in this project yet.");
  }
  const float list_h = std::min(
      220.0f, ImGui::GetTextLineHeightWithSpacing() *
                  static_cast<float>(std::max<std::size_t>(scripts_.size(), 1)) +
              8.0f);
  ImGui::BeginChild("##script_list", ImVec2(0, list_h), ImGuiChildFlags_Borders);
  for (const std::string& rel : scripts_) {
    if (ImGui::Selectable(rel.c_str(), rel == scripts_selected_,
                          ImGuiSelectableFlags_AllowDoubleClick)) {
      scripts_selected_ = rel;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        open_script(rel);
      }
    }
  }
  ImGui::EndChild();

  if (scripts_selected_.empty() ||
      std::find(scripts_.begin(), scripts_.end(), scripts_selected_) ==
          scripts_.end()) {
    return;
  }
  const std::string rel = scripts_selected_;
  if (rel.size() > 3 && rel.compare(rel.size() - 3, 3, ".cs") == 0) {
    // C# without the SDK: say how to switch it on (the Telegraph says it too).
    static int cs_ok = -1;
    if (cs_ok < 0) cs_ok = runtime::csharp_scripts::sdk_available(nullptr) ? 1 : 0;
    if (!cs_ok) {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::Warning());
      ImGui::TextWrapped("C# scripts are disabled here: install the .NET 8 SDK to enable C#.");
      ImGui::PopStyleColor();
      ImGui::TextDisabled("https://dotnet.microsoft.com/download/dotnet/8.0");
    }
  }
  const ScriptInfo info = script_info(rel);
  if (info.ok) {
    std::string props;
    for (const ScriptProp& p : info.defaults) {
      props += (props.empty() ? "" : ", ") + p.name + " = " + describe(p.value);
    }
    ImGui::TextColored(theme::Success(), "Rides clean.");
    if (!props.empty()) {
      ImGui::TextWrapped("props: %s", props.c_str());
    }
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Danger());
    ImGui::TextWrapped("%s", info.error.c_str());
    ImGui::PopStyleColor();
  }
  if (theme::SecondaryButton("Open in Editor", ImVec2(120, 0))) {
    open_script(rel);
  }
  const Entity2D* target = workspace_.selected();
  const bool can_attach = !playing && target && !target->tilemap &&
                          (!target->script || target->script->path != rel);
  ImGui::SameLine();
  ImGui::BeginDisabled(!can_attach);
  char attach[96];
  std::snprintf(attach, sizeof(attach), "Attach to %s",
                target ? target->name.c_str() : "selection");
  if (theme::CopperButton(attach)) {
    ScriptData s;
    s.path = rel;
    if (target->script) s.props = target->script->props;
    set_script(target->id, s, "Attach Script");
  }
  ImGui::EndDisabled();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
