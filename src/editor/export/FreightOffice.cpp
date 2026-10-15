#include "editor/export/FreightOffice.h"

#include "editor/launch/GameLauncher.h"
#include "editor/launch/OpenWithOs.h"
#include "editor/ui/Theme.h"
#include "vfs/Pak.h"

#include <imgui.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace ts {
namespace tombstone {
namespace editor {

namespace fs = std::filesystem;

namespace {
void copy_to(char* dst, std::size_t n, const std::string& s) {
  std::strncpy(dst, s.c_str(), n - 1);
  dst[n - 1] = '\0';
}

std::string default_out_dir() {
#if defined(_WIN32)
  if (const char* h = std::getenv("USERPROFILE")) return (fs::path(h) / "TombStoneExports").string();
#else
  if (const char* h = std::getenv("HOME")) return (fs::path(h) / "TombStoneExports").string();
#endif
  return "TombStoneExports";
}

void line(const ImVec4& color, const char* tag, const std::string& text) {
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextUnformatted(tag);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextWrapped("%s", text.c_str());
}

void report_lines(const exporter::ValidationReport& r) {
  for (const std::string& e : r.errors) line(theme::Danger(), "HELD ", e);
  for (const std::string& w : r.warnings) line(theme::Warning(), "MIND ", w);
  for (const std::string& n : r.notes) line(theme::TextMuted(), "NOTE ", n);
}
}  // namespace

FreightOffice& FreightOffice::get() {
  static FreightOffice office;
  return office;
}

void FreightOffice::reset_for(const std::string& project_dir, const std::string& project_name) {
  project_dir_ = project_dir;
  project_name_ = project_name;
  copy_to(name_, sizeof(name_), project_name);
  copy_to(version_, sizeof(version_), "1.0.0");
  copy_to(exe_, sizeof(exe_), exporter::slugify(project_name));
  std::error_code ec;
  copy_to(icon_, sizeof(icon_), fs::exists(fs::path(project_dir) / "icon.png", ec) ? "icon.png" : "");
  if (out_dir_[0] == '\0') copy_to(out_dir_, sizeof(out_dir_), default_out_dir());
  inspected_ = shipped_ = false;
  report_ = {};
  result_ = {};
  runtime_ = launcher::find_game_executable();
}

void FreightOffice::open(const std::string& project_dir, const std::string& project_name) {
  if (project_dir != project_dir_) reset_for(project_dir, project_name);
  open_ = true;
  inspect();
}

void FreightOffice::inspect() {
  report_ = exporter::validate_project(project_dir_, profile_ == 1 ? "dev" : "shipping");
  inspected_ = true;
}

void FreightOffice::ship() {
  exporter::ExportOptions opt;
  opt.project_dir = project_dir_;
  opt.out_dir = out_dir_;
  opt.profile = profile_ == 1 ? "dev" : "shipping";
  opt.runtime_exe = runtime_;
  opt.server = server_;
  opt.zip = zip_;
  opt.name = name_;
  opt.version = version_;
  opt.exe_name = exe_;
  opt.icon = icon_;
  opt.window_width = width_;
  opt.window_height = height_;
  exporter::export_project(opt, &result_);
  shipped_ = true;
}

void FreightOffice::draw() {
  if (!open_) return;
  ImGui::SetNextWindowSize(ImVec2(620, 640), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Freight Office###ts_freight_office", &open_)) {
    ImGui::End();
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, theme::Accent());
  ImGui::TextUnformatted("FREIGHT OFFICE");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("crate up \"%s\" for the stagecoach", project_name_.c_str());
  ImGui::Separator();

  ImGui::SeparatorText("Waybill");
  ImGui::InputText("Game name", name_, sizeof(name_));
  ImGui::InputText("Version", version_, sizeof(version_));
  ImGui::InputText("Executable", exe_, sizeof(exe_));
  ImGui::InputText("Icon (png in project)", icon_, sizeof(icon_));
  ImGui::InputInt("Window width", &width_, 16, 128);
  ImGui::InputInt("Window height", &height_, 16, 128);
  const char* profiles[] = {"Shipping (strict, editor data stripped)", "Dev (Debug-ish, warnings only, log)"};
  if (ImGui::Combo("Profile", &profile_, profiles, 2)) inspect();
  ImGui::Checkbox("Dedicated server (ts_server)", &server_);
  ImGui::SameLine();
  ImGui::Checkbox("Zip it", &zip_);

  ImGui::SeparatorText("Depot");
  ImGui::InputText("Output folder", out_dir_, sizeof(out_dir_));
  ImGui::TextDisabled("Game lands in %s", (fs::path(out_dir_) / exporter::slugify(name_)).string().c_str());
  if (runtime_.empty()) {
    line(theme::Danger(), "HELD ", "ts_game is not built next to this editor; build it (Release) first.");
  } else {
    ImGui::TextDisabled("Runtime: %s", runtime_.c_str());
  }

  ImGui::Spacing();
  if (ImGui::Button("Inspect cargo")) inspect();
  ImGui::SameLine();
  ImGui::BeginDisabled(runtime_.empty());
  ImGui::PushStyleColor(ImGuiCol_Button, theme::Accent());
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::AccentHover());
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextOnAccent());
  if (ImGui::Button("Ship It")) ship();
  ImGui::PopStyleColor(3);
  ImGui::EndDisabled();
  if (shipped_ && result_.ok) {
    ImGui::SameLine();
    if (ImGui::Button("Open depot folder")) launcher::open_with_os(result_.game_dir);
  }

  ImGui::SeparatorText("Manifest");
  ImGui::BeginChild("freight_report", ImVec2(0, 0), ImGuiChildFlags_Borders);
  if (shipped_) {
    line(result_.ok ? theme::Success() : theme::Danger(), result_.ok ? "SHIPPED" : "HELD AT THE DEPOT", "");
    if (result_.ok) {
      ImGui::TextWrapped("Folder: %s", result_.game_dir.c_str());
      ImGui::TextWrapped("Runtime: %s", result_.exe_path.c_str());
      ImGui::TextWrapped("game.tspak: %zu files, %llu bytes, hash %s", result_.pak_files,
                         static_cast<unsigned long long>(result_.pak_bytes),
                         vfs::hex64(result_.pak_hash).c_str());
      if (!result_.zip_path.empty()) ImGui::TextWrapped("Zip: %s", result_.zip_path.c_str());
      for (const std::string& p : result_.plugins_shipped) ImGui::BulletText("%s", p.c_str());
    }
    report_lines(result_.report);
  } else if (inspected_) {
    line(report_.ok() ? theme::Success() : theme::Danger(),
         report_.ok() ? "CARGO SOUND" : "CARGO HELD", "");
    report_lines(report_);
  }
  ImGui::EndChild();
  ImGui::End();
}

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
