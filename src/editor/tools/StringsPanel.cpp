#include "editor/tools/StringsPanel.h"
#include "editor/ux/Tooltip.h"

#include "editor/ui/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {
std::vector<std::string> g_pending_fonts;
std::set<std::string> g_merged_fonts;

std::string read_all(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Multi-line text input over a std::string.
bool input_multiline(const char* label, std::string* s, const ImVec2& size) {
  return ImGui::InputTextMultiline(
      label, s->data(), s->capacity() + 1, size, ImGuiInputTextFlags_CallbackResize,
      [](ImGuiInputTextCallbackData* d) {
        if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
          auto* str = static_cast<std::string*>(d->UserData);
          str->resize(static_cast<std::size_t>(d->BufTextLen));
          d->Buf = str->data();
        }
        return 0;
      },
      s);
}
}  // namespace

StringsPanel& StringsPanel::get() {
  static StringsPanel p;
  return p;
}

void StringsPanel::request_fonts(const std::string& project_dir) {
  for (const std::string& f : loc::fallback_fonts(project_dir)) {
    if (!g_merged_fonts.count(f)) g_pending_fonts.push_back(f);
  }
}

void StringsPanel::apply_pending_fonts() {
  if (g_pending_fonts.empty() || !ImGui::GetCurrentContext()) return;
  ImGuiIO& io = ImGui::GetIO();
  if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
  for (const std::string& f : g_pending_fonts) {
    if (g_merged_fonts.count(f)) continue;
    g_merged_fonts.insert(f);
    ImFontConfig cfg;
    cfg.MergeMode = true;
    io.Fonts->AddFontFromFileTTF(f.c_str(), 0.0f, &cfg);
  }
  g_pending_fonts.clear();
}

std::set<std::string> StringsPanel::scan_used_keys(const std::string& project_dir) {
  std::set<std::string> keys;
  static const std::regex call(R"((?:\btr|\bTr)\s*\(\s*["']([^"'\\]+)["'])");
  static const std::regex text_key(R"("text_key"\s*:\s*"([^"\\]+)\")");
  std::error_code ec;
  for (const char* sub : {"scripts", "ui"}) {
    const fs::path dir = fs::path(project_dir) / sub;
    if (!fs::is_directory(dir, ec)) continue;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const std::string ext = it->path().extension().string();
      if (ext != ".lua" && ext != ".json" && ext != ".cpp" && ext != ".h" && ext != ".cs") continue;
      const std::string text = read_all(it->path());
      for (const std::regex* re : {&call, &text_key}) {
        for (std::sregex_iterator m(text.begin(), text.end(), *re), end; m != end; ++m) keys.insert((*m)[1]);
      }
    }
  }
  return keys;
}

void StringsPanel::open(const std::string& project_dir, const std::string& project_name) {
  if (project_dir != project_dir_ || !open_) {
    project_dir_ = project_dir;
    project_name_ = project_name;
    reload();
  }
  open_ = true;
}

void StringsPanel::reload() {
  std::string errors;
  tables_ = loc::StringTables{};
  tables_.load_project(project_dir_, &errors);
  used_ = scan_used_keys(project_dir_);
  status_ = errors.empty() ? "" : errors;
  dirty_ = false;
  edit_key_.clear();
  request_fonts(project_dir_);
  if (selected_.empty()) {
    const auto k = tables_.keys();
    if (!k.empty()) selected_ = k.front();
  }
}

void StringsPanel::save() {
  std::string err;
  if (tables_.save_project(project_dir_, &err)) {
    status_ = "Saved strings/ (" + std::to_string(tables_.languages().size()) + " languages)";
    dirty_ = false;
    // Running previews pick the new text up.
    if (loc::project_dir() == project_dir_) {
      const std::string lang = loc::language();
      loc::set_tables(tables_);
      loc::set_language(lang);
    }
  } else {
    status_ = err;
  }
}

int StringsPanel::problem_count() const {
  int n = 0;
  for (const std::string& l : tables_.languages()) n += static_cast<int>(tables_.missing(l).size());
  const auto keys = tables_.keys();
  for (const std::string& k : used_) {
    if (!std::binary_search(keys.begin(), keys.end(), k)) ++n;
  }
  return n;
}

void StringsPanel::draw() {
  if (!open_) return;
  if (pinned_) {
    ImGui::SetNextWindowPos(ImVec2(pin_[0], pin_[1]), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pin_[2], pin_[3]), ImGuiCond_Always);
  } else {
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
  }
  if (!ImGui::Begin("Strings (Localisation)###ts_strings_panel", &open_)) {
    ImGui::End();
    return;
  }
  const std::vector<std::string> langs = tables_.languages();
  const std::vector<std::string> defined = tables_.keys();
  std::vector<std::string> keys = defined;
  keys.insert(keys.end(), used_.begin(), used_.end());
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  auto is_defined = [&](const std::string& k) { return std::binary_search(defined.begin(), defined.end(), k); };

  ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
  ImGui::TextUnformatted("STRINGS");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("%zu keys, %zu languages, base \"%s\"", keys.size(), langs.size(), tables_.base.c_str());
  const int problems = problem_count();
  ImGui::SameLine();
  if (problems > 0) {
    ImGui::TextColored(theme::Danger(), "  %d missing", problems);
  } else if (!keys.empty()) {
    ImGui::TextColored(theme::Success(), "  all translated");
  }

  // Toolbar.
  ImGui::SetNextItemWidth(180);
  ImGui::InputTextWithHint("##filter", "filter keys / text", filter_, sizeof(filter_));
  ImGui::SameLine();
  ImGui::Checkbox("Missing only", &missing_only_);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140);
  const std::string cur = loc::language();
  if (ImGui::BeginCombo("Preview", loc::display_name(cur).c_str())) {
    for (const std::string& l : langs) {
      if (ImGui::Selectable((l + "  " + loc::display_name(l)).c_str(), l == cur)) {
        if (loc::project_dir() != project_dir_ || !loc::tables().has_language(l)) loc::set_tables(tables_);
        loc::set_language(l);
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!dirty_);
  ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent());
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::AccentHover());
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextOnAccent());
  if (ImGui::Button("Save")) save();
  ImGui::PopStyleColor(3);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Reload")) reload();

  ImGui::SetNextItemWidth(180);
  ImGui::InputTextWithHint("##newkey", "new.key", new_key_, sizeof(new_key_));
  ImGui::SameLine();
  if (ImGui::Button("+ Key") && new_key_[0]) {
    tables_.set(tables_.base, new_key_, new_key_);
    selected_ = new_key_;
    new_key_[0] = '\0';
    dirty_ = true;
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70);
  ImGui::InputTextWithHint("##newlang", "fr", new_lang_, sizeof(new_lang_));
  ImGui::SameLine();
  if (ImGui::Button("+ Language") && new_lang_[0]) {
    tables_.add_language(new_lang_, "json");
    new_lang_[0] = '\0';
    dirty_ = true;
  }
  if (!status_.empty()) ImGui::TextColored(theme::TextMuted(), "%s", status_.c_str());

  // Grid.
  const float editor_h = 150.0f;
  const int cols = 1 + static_cast<int>(langs.size());
  if (ImGui::BeginTable("##strings", std::max(cols, 1),
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0, -editor_h))) {
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    for (const std::string& l : langs) {
      const int miss = static_cast<int>(tables_.missing(l).size());
      const std::string head = l + (l == tables_.base ? " (base)" : "") + (miss ? "  -" + std::to_string(miss) : "");
      ImGui::TableSetupColumn(head.c_str(), ImGuiTableColumnFlags_WidthStretch, 1.4f);
    }
    ImGui::TableHeadersRow();
    const std::string filter = filter_;
    for (const std::string& k : keys) {
      bool any_missing = !is_defined(k);
      for (const std::string& l : langs) {
        const std::string* t = tables_.find(l, k);
        if (!t || t->empty()) any_missing = true;
      }
      if (missing_only_ && !any_missing) continue;
      if (!filter.empty()) {
        bool hit = k.find(filter) != std::string::npos;
        for (const std::string& l : langs) {
          const std::string* t = tables_.find(l, k);
          if (t && t->find(filter) != std::string::npos) hit = true;
        }
        if (!hit) continue;
      }
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      if (!is_defined(k)) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, theme::U32(theme::Danger(), 0.35f));
      }
      if (ImGui::Selectable((k + "##key").c_str(), selected_ == k, ImGuiSelectableFlags_SpanAllColumns)) {
        selected_ = k;
      }
      if (!is_defined(k) && ImGui::IsItemHovered()) {
        ux::SetTooltip("Used by a script / UI document but defined in no table");
      }
      for (std::size_t c = 0; c < langs.size(); ++c) {
        ImGui::TableSetColumnIndex(static_cast<int>(c) + 1);
        const std::string* t = tables_.find(langs[c], k);
        if (!t || t->empty()) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, theme::U32(theme::Danger(), 0.22f));
          ImGui::TextColored(theme::Danger(), "missing");
        } else {
          std::string one = *t;
          if (const auto nl = one.find('\n'); nl != std::string::npos) one = one.substr(0, nl) + " ...";
          ImGui::TextUnformatted(one.c_str());
        }
      }
    }
    ImGui::EndTable();
  }

  // Editor for the selected key.
  ImGui::SeparatorText(selected_.empty() ? "Select a key" : ("Edit  " + selected_).c_str());
  if (!selected_.empty()) {
    if (edit_key_ != selected_ || edit_buf_.size() != langs.size()) {
      edit_key_ = selected_;
      edit_buf_.clear();
      for (const std::string& l : langs) {
        const std::string* t = tables_.find(l, selected_);
        edit_buf_.push_back(t ? *t : std::string());
      }
    }
    ImGui::BeginChild("##edit", ImVec2(0, 0));
    for (std::size_t c = 0; c < langs.size(); ++c) {
      ImGui::PushID(static_cast<int>(c));
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(edit_buf_[c].empty() ? theme::Danger() : theme::Sand(), "%-6s", langs[c].c_str());
      ImGui::SameLine(70);
      edit_buf_[c].reserve(256);
      if (input_multiline("##t", &edit_buf_[c], ImVec2(-1, ImGui::GetTextLineHeight() * 2.2f))) {
        tables_.set(langs[c], selected_, edit_buf_[c]);
        dirty_ = true;
      }
      ImGui::PopID();
    }
    if (ImGui::Button("Delete key")) {
      tables_.remove_key(selected_);
      selected_.clear();
      dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Lua: tr(\"%s\")   UI: \"text_key\": \"%s\"   C++: tr(\"%s\")   C#: Tr(\"%s\")",
                        edit_key_.c_str(), edit_key_.c_str(), edit_key_.c_str(), edit_key_.c_str());
    ImGui::EndChild();
  }
  ImGui::End();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
