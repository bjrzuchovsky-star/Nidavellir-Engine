#pragma once

// The Freight Office (File > Export...): the editor face of ts_export.
// Pick a profile, name, version, window, icon and an output folder, inspect
// the cargo (validation report), then Ship It: Release ts_game + DLLs +
// game.json + cooked game.tspak (+ server, + portable zip, + Windows
// setup installer). Distribution settings are saved in the project. One shared window for
// the 2D and 3D/VR editors.

#include "export/Exporter.h"
#include "export/Installer.h"
#include "export/Provenance.h"

#include <vector>

#include <string>

namespace ts {
namespace tombstone {
namespace editor {

class FreightOffice {
 public:
  static FreightOffice& get();

  // Opens the window on a project (keeps its last settings per project).
  void open(const std::string& project_dir, const std::string& project_name);
  bool is_open() const { return open_; }
  // Call every frame from the active editor screen.
  void draw();
  // Pins the window (headless screenshots: ts_admin --freight-shot).
  void pin(float x, float y, float w, float h) {
    pinned_ = true;
    pin_[0] = x; pin_[1] = y; pin_[2] = w; pin_[3] = h;
  }

 private:
  void reset_for(const std::string& project_dir, const std::string& project_name);
  void inspect();
  void ship();
  void save_settings();
  void draw_distribution();
  void draw_rights();  // third-party content that would ship (export/Provenance.h)

  bool open_ = false;
  std::string project_dir_;
  std::string project_name_;
  char name_[96] = {};
  char version_[32] = {};
  char exe_[64] = {};
  char icon_[160] = {};
  char out_dir_[512] = {};
  int width_ = 1280;
  int height_ = 720;
  int profile_ = 0;  // 0 shipping, 1 dev
  bool server_ = false;
  bool zip_ = false;
  bool inspected_ = false;
  exporter::ValidationReport report_;
  bool shipped_ = false;
  exporter::ExportResult result_;
  std::string runtime_;
  // Distribution (export_settings.json in the project).
  char publisher_[128] = {};
  char license_[260] = {};
  int install_mode_ = 0;  // 0 ask, 1 per-user, 2 all users
  bool installer_ = false;
  bool start_menu_ = true;
  bool desktop_icon_ = true;
  std::string iscc_;
  std::string settings_status_;
  bool pinned_ = false;
  float pin_[4] = {};
  std::vector<exporter::provenance::Shipped> third_party_;
  bool rights_all_ = false;
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
