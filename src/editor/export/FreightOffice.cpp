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

void FreightOffice::draw_distribution() {
  ImGui::SeparatorText("Distribution");
  ImGui::Checkbox("Portable zip", &zip_);
  ImGui::SameLine();
  ImGui::Checkbox("Windows setup installer", &installer_);
  ImGui::BeginDisabled(!installer_);
  ImGui::InputText("License / EULA (txt or rtf)", license_, sizeof(license_));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Project-relative file; shown as the installer's license page");
  const char* modes[] = {"Ask the player (per-user default)", "Per-user (no admin)", "All users (admin)"};
  ImGui::Combo("Install for", &install_mode_, modes, 3);
  ImGui::Checkbox("Start menu shortcut", &start_menu_);
  ImGui::SameLine();
  ImGui::Checkbox("Desktop shortcut", &desktop_icon_);
  ImGui::EndDisabled();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::TextMuted());
  ImGui::TextWrapped("Ships the VC++ runtime app-local (no redistributable), an uninstaller, "
                     "and an OpenGL 3.3 check with a friendly message.");
  ImGui::PopStyleColor();
  if (installer_) {
    if (iscc_.empty()) {
      line(theme::Warning(), "MIND ", "Inno Setup not found: the .iss script is written but no setup.exe "
                                     "(run tools/installer/install_inno.ps1, or set TS_ISCC).");
    } else {
      ImGui::TextDisabled("Inno Setup: %s", iscc_.c_str());
    }
  }
  if (ImGui::Button("Save settings")) save_settings();
  ImGui::SameLine();
  ImGui::TextDisabled("%s", settings_status_.empty() ? "kept in the project (export_settings.json)"
                                                     : settings_status_.c_str());
}

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
  // Saved distribution settings win over the defaults above.
  const exporter::ExportSettings st = exporter::load_settings(project_dir);
  if (!st.version.empty()) copy_to(version_, sizeof(version_), st.version);
  if (!st.icon.empty()) copy_to(icon_, sizeof(icon_), st.icon);
  copy_to(publisher_, sizeof(publisher_), st.publisher);
  copy_to(license_, sizeof(license_), st.license);
  install_mode_ = st.install_mode == "user" ? 1 : st.install_mode == "all" ? 2 : 0;
  installer_ = st.installer;
  zip_ = st.portable_zip;
  start_menu_ = st.start_menu;
  desktop_icon_ = st.desktop_icon;
  settings_status_.clear();
  iscc_ = exporter::find_iscc();
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
  third_party_ = exporter::provenance::shipped_third_party(project_dir_, exporter::content_files(project_dir_));
  exporter::provenance::Ledger l;
  exporter::provenance::load(project_dir_, &l);
  rights_all_ = !l.acknowledged_all.empty();
}

void FreightOffice::draw_rights() {
  if (third_party_.empty()) return;
  ImGui::SeparatorText("Rights");
  int open = 0;
  for (const auto& s : third_party_) open += s.acknowledged ? 0 : 1;
  ImGui::PushStyleColor(ImGuiCol_Text, open ? theme::Warning() : theme::TextMuted());
  ImGui::TextWrapped("%zu imported third-party file(s) (Source engine formats / .gma addons) will ship. "
                     "You need the rights to ship these. TombStone is not affiliated with Valve or Facepunch.",
                     third_party_.size());
  ImGui::PopStyleColor();
  bool changed = false;
  if (ImGui::Checkbox("I have the rights to ship all of these (this claim)", &rights_all_)) {
    exporter::provenance::acknowledge(project_dir_, "", rights_all_);
    changed = true;
  }
  if (ImGui::TreeNode("rights_list", "Per asset (%d not acknowledged)", open)) {
    for (const auto& s : third_party_) {
      bool ack = s.acknowledged;
      ImGui::BeginDisabled(rights_all_);
      if (ImGui::Checkbox(("##ack" + s.path).c_str(), &ack)) {
        exporter::provenance::acknowledge(project_dir_, s.path, ack);
        changed = true;
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::TextWrapped("%s  [%s%s]", s.path.c_str(), s.origin.c_str(),
                         s.author.empty() ? "" : (", by " + s.author).c_str());
    }
    ImGui::TreePop();
  }
  if (changed) inspect();
}

void FreightOffice::save_settings() {
  exporter::ExportSettings st;
  st.icon = icon_;
  st.version = version_;
  st.publisher = publisher_;
  st.license = license_;
  st.install_mode = install_mode_ == 1 ? "user" : install_mode_ == 2 ? "all" : "ask";
  st.installer = installer_;
  st.portable_zip = zip_;
  st.start_menu = start_menu_;
  st.desktop_icon = desktop_icon_;
  std::string err;
  settings_status_ = exporter::save_settings(project_dir_, st, &err)
                         ? std::string("Saved to ") + exporter::kSettingsFile
                         : err;
}

void FreightOffice::ship() {
  save_settings();
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
  opt.publisher = publisher_;
  opt.license = license_;
  opt.installer = installer_;
  opt.install_mode = install_mode_ == 1 ? "user" : install_mode_ == 2 ? "all" : "ask";
  opt.start_menu = start_menu_;
  opt.desktop_icon = desktop_icon_;
  exporter::export_project(opt, &result_);
  shipped_ = true;
}

void FreightOffice::draw() {
  if (!open_) return;
  if (pinned_) {
    ImGui::SetNextWindowPos(ImVec2(pin_[0], pin_[1]), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pin_[2], pin_[3]), ImGuiCond_Always);
  } else {
    ImGui::SetNextWindowSize(ImVec2(640, 780), ImGuiCond_FirstUseEver);
  }
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
  ImGui::InputText("Publisher", publisher_, sizeof(publisher_));
  ImGui::InputInt("Window width", &width_, 16, 128);
  ImGui::InputInt("Window height", &height_, 16, 128);
  const char* profiles[] = {"Shipping (strict, editor data stripped)", "Dev (Debug-ish, warnings only, log)"};
  if (ImGui::Combo("Profile", &profile_, profiles, 2)) inspect();
  ImGui::Checkbox("Dedicated server (ts_server)", &server_);

  draw_distribution();
  draw_rights();

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
      if (!result_.zip_path.empty()) ImGui::TextWrapped("Portable zip: %s", result_.zip_path.c_str());
      if (!result_.installer_path.empty()) {
        ImGui::TextWrapped("Installer: %s", result_.installer_path.c_str());
      } else if (!result_.iss_path.empty()) {
        ImGui::TextWrapped("Installer script: %s", result_.iss_path.c_str());
      }
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
