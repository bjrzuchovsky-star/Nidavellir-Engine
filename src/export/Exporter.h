#pragma once

// ts_export: "Ship It". Turns a TombStone project folder into a standalone
// game folder (and optionally a zip):
//
//   <out>/<slug>/
//     <Exe>(.exe)        ts_game (Release), renamed; Windows: icon stamped
//     <slug>.sh          Linux launcher (LD_LIBRARY_PATH=lib)
//     game.json          manifest: name, version, icon, window, start scene,
//                        dimension, profile, pak
//     game.tspak         cooked content: assets / scripts / ui / scenes /
//                        project.json (editor-only data stripped)
//     plugins/           dynamic plugin DLLs the project enables
//     lib/               Linux shared libs (OpenXR loader) when needed
//     server/            optional dedicated server (ts_server + loose content)
//     README.txt         how to run + MSVC runtime note
//     export_report.txt  validation report
//
// Profiles: "shipping" (strict: script errors and a missing scene fail the
// export; editor data stripped) and "dev" (Debug-ish: warnings only, keeps
// .pdb files, launcher keeps a Telegraph log, no stripping).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace exporter {

struct ExportOptions {
  std::string project_dir;
  std::string out_dir;              // the game lands in <out_dir>/<slug>
  std::string profile = "shipping"; // "shipping" | "dev"
  std::string runtime_exe;          // ts_game to ship ("" = look near runtime_dir)
  std::string server_exe;           // ts_server ("" = sibling of ts_game)
  bool server = false;              // also package a dedicated server
  bool zip = false;                 // also write <out_dir>/<slug>-<version>.zip
  std::string name;                 // "" = project.json name
  std::string version;              // "" = project.json version, else 1.0.0
  std::string exe_name;             // "" = slug
  std::string icon;                 // project-relative png ("" = auto: icon.png)
  int window_width = 0;             // 0 = project.json / 1280
  int window_height = 0;            // 0 = project.json / 720
};

struct ValidationReport {
  std::vector<std::string> errors;
  std::vector<std::string> warnings;
  std::vector<std::string> notes;
  bool ok() const { return errors.empty(); }
};

struct ExportResult {
  bool ok = false;
  std::string game_dir;      // <out>/<slug>
  std::string exe_path;
  std::string pak_path;
  std::string zip_path;
  std::uint64_t pak_hash = 0;
  std::size_t pak_files = 0;
  std::uint64_t pak_bytes = 0;
  std::vector<std::string> plugins_shipped;
  ValidationReport report;
  std::string text() const;  // human report (also export_report.txt)
};

using Progress = std::function<void(const std::string& line)>;

// Validation alone (Freight Office "Inspect" button): scene present, Lua
// compiles, referenced asset files exist, plugin DLLs found.
ValidationReport validate_project(const std::string& project_dir,
                                  const std::string& profile);

bool export_project(const ExportOptions& opt, ExportResult* out,
                    const Progress& progress = {});

// Files of a project that go in the pak (relative, '/' separated), with
// editor-only data skipped (dot folders, exports, builds, imgui.ini, *.bak,
// thumbnails, plugin binaries).
std::vector<std::string> content_files(const std::string& project_dir);
bool is_editor_only(const std::string& rel);

// Lowercase dash slug of a display name ("Ghost Town 3D" -> "ghost-town-3d").
std::string slugify(const std::string& name);

}  // namespace exporter
}  // namespace tombstone
}  // namespace ts
