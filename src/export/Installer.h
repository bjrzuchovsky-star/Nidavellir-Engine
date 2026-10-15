#pragma once

// Distribution for the Freight Office: per-project export settings
// (export_settings.json), a Windows setup installer built with Inno Setup
// (ISCC), a PNG -> .ico writer and a PE import check that proves an
// exported game folder needs nothing from the dev machine.

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace exporter {

// Saved per project in <project>/export_settings.json (never shipped).
struct ExportSettings {
  std::string icon;                  // project-relative png ("" = auto icon.png)
  std::string version;               // "" = project.json version / 1.0.0
  std::string publisher;             // shown in Add/Remove Programs
  std::string license;               // project-relative .txt/.rtf EULA ("" = no page)
  std::string install_mode = "ask";  // "ask" | "user" | "all"
  bool installer = false;            // build <slug>-<version>-setup.exe
  bool portable_zip = true;          // build <slug>-<version>.zip
  bool start_menu = true;
  bool desktop_icon = true;
};

ExportSettings load_settings(const std::string& project_dir);
bool save_settings(const std::string& project_dir, const ExportSettings& s, std::string* error);
constexpr const char* kSettingsFile = "export_settings.json";

// Writes a single-image .ico (PNG entry, Vista+) from a PNG.
bool write_ico_from_png(const std::string& png, const std::string& ico, std::string* error);

// ISCC.exe: $TS_ISCC, PATH, Program Files (x86) / Program Files / per-user
// Inno Setup 6. "" when absent.
std::string find_iscc();

struct InstallerSpec {
  std::string game_dir;      // exported folder (what gets installed)
  std::string out_dir;       // where the setup exe lands
  std::string name;
  std::string version;
  std::string publisher;
  std::string exe_name;      // file name inside game_dir
  std::string slug;
  std::string icon_ico;      // absolute .ico ("" = none)
  std::string license_file;  // absolute ("" = no license page)
  std::string install_mode = "ask";
  bool start_menu = true;
  bool desktop_icon = true;
};

// The Inno Setup script (pure text; testable anywhere).
std::string inno_script(const InstallerSpec& s);

// Writes <out_dir>/<slug>-installer.iss and runs ISCC. On success sets
// *setup_exe. ISCC missing -> false with "ISCC not found" in *error.
bool build_installer(const InstallerSpec& s, std::string* iss_path, std::string* setup_exe,
                     std::string* log, std::string* error);

// PE imports of a .exe/.dll (empty + error on a non-PE file).
std::vector<std::string> pe_imports(const std::string& path, std::string* error);
// True for DLLs every Windows 10+ PC has (KERNEL32, USER32, OPENGL32,
// api-ms-win-*, ucrtbase ...).
bool is_system_dll(const std::string& dll);
// Walks the exe + every DLL in the folder: any import that is neither a
// system DLL nor shipped in the folder lands in *missing.
bool check_folder_dependencies(const std::string& game_dir, std::vector<std::string>* missing,
                               std::vector<std::string>* shipped_dlls, std::string* error);

}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
