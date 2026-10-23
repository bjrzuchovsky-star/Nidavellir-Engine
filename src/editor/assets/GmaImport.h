#pragma once

// Source engine addon (.gma) import - "Import Source addon (.gma)..." in
// the Asset Browser, files dropped on the editor, ts_admin --import-gma.
//
// open() reads the archive (LZMA-compressed downloads too), checks every
// file's CRC32 and lists the contents by category (models / materials /
// sound / other) for the picker. extract() writes the selected files into
// the claim under assets/Models/<Addon>/ keeping their Source-relative
// paths (models/..., materials/..., sound/...), so the model importer
// finds materials next to the models (render3d/import/SourceMaterials.h).
// A selected .mdl brings its .vvd / .vtx / .phy / .ani siblings. Existing
// files are never overwritten. Every written file is tagged in the claim's
// provenance ledger (origin "gma:<addon>", author) - see
// export/Provenance.h. The next Asset Browser scan imports the models,
// .vtf textures and sounds.
//
// Clean-room (see render3d/import/Gma.h); only content the user has the
// rights to should be imported.

#include "editor/assets/AssetDb.h"
#include "editor/ui/FolderBrowser.h"
#include "render3d/import/Gma.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace gmaimport {

struct Item {
  std::string name;      // path inside the addon
  std::string category;  // "models" / "materials" / "sound" / "other"
  std::uint64_t size = 0;
  bool crc_ok = true;
  bool safe = true;      // path safe to extract
  bool selected = true;
};

struct Plan {
  std::string gma_path;
  render3d::gma::Archive archive;
  std::vector<Item> items;
  bool archive_crc_ok = true;
  int bad_crc = 0;
};

bool open(const std::string& gma_path, Plan* out, std::string* error);
// Folder name for the addon ("Crate Pack!" -> "Crate_Pack"), from the
// addon name or the file name.
std::string addon_folder_name(const Plan& plan);
// "assets/Models/<Addon>" (with _2, _3... when it exists already).
std::string default_destination(const std::string& claim_dir, const Plan& plan);
// Select / deselect a whole category ("" = everything).
void select(Plan* plan, const std::string& category, bool on);

struct Result {
  std::string dest;                    // project-relative folder
  std::vector<std::string> written;    // project-relative files
  std::vector<std::string> models;     // .mdl files among them
  std::vector<std::string> skipped;    // "name: why"
  std::vector<std::string> errors;
};
// dest_rel "" = default_destination. False when nothing was written.
bool extract(const std::string& claim_dir, const Plan& plan, const std::string& dest_rel, Result* out);

// Asset Browser details for a Source .mdl: each material's status (found
// .vmt / .vtf, or where it was looked for) with Relink... (a .vmt / .vtf /
// image picked by hand, saved in the model's settings
// "material_overrides") and Clear. True when the settings changed (the
// caller reimports and reloads the model).
bool draw_source_materials(assetdb::AssetDb& db, const assetdb::AssetDb::Entry& e, FolderBrowser* picker,
                           std::string* picking_for);

// Editor picker (ImGui). open_file() starts on a .gma, open_picker() asks
// for one first. draw() every frame; true once files were extracted
// (then rescan the asset database).
class Dialog {
 public:
  void open_picker(const std::string& claim_dir);
  void open_file(const std::string& claim_dir, const std::string& gma_path);
  bool draw();
  const Result& result() const { return result_; }
  bool is_open() const { return open_ || picking_; }

 private:
  std::string claim_;
  Plan plan_;
  std::string error_;
  Result result_;
  bool open_ = false;
  bool picking_ = false;
  bool request_popup_ = false;
  char dest_[512] = "";
  FolderBrowser browser_;
};

}  // namespace gmaimport
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
