#pragma once

// The Freight Office (File > Export...): the editor face of ts_export.
// Pick a profile, name, version, window, icon and an output folder, inspect
// the cargo (validation report), then Ship It: Release ts_game + DLLs +
// game.json + cooked game.tspak (+ server, + zip). One shared window for
// the 2D and 3D/VR editors.

#include "export/Exporter.h"

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

 private:
  void reset_for(const std::string& project_dir, const std::string& project_name);
  void inspect();
  void ship();

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
};

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
