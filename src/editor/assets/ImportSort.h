#pragma once

// Auto-sorted imports. Files brought into a claim from outside (dropped on
// the editor window, Asset Browser > Import..., the 2D Supply Wagon image
// import, a Vault pull) land in type folders by default:
//
//   assets/Models/<ModelName>/   a model, its sibling files (.bin, .mtl,
//                                .vvd...) and its textures, keeping their
//                                relative layout (so the model's texture
//                                paths keep working)
//   assets/Textures/  assets/Materials/  assets/Audio/  assets/Fonts/
//   assets/Scenes/  assets/Animations/  assets/Other/
//   prefabs/  scripts/           the claim's existing top-level folders
//                                (prefab library, script builds)
//
// Per-claim settings in <claim>/.tombstone/import.json:
//   {"sort_imports": true, "keep_model_packs": true}
// sort_imports off: everything lands in assets/ (models still get their
// own Models-style folder layout relative to the model). keep_model_packs
// off: a model's textures go to assets/Textures/<ModelName>/ instead and
// the model's texture paths are rewritten (glTF uris, OBJ .mtl maps);
// formats whose paths can't be rewritten (FBX, DAE...) keep them with the
// model. Dropping on a specific Asset Browser folder overrides sorting.
// The .meta files go to .tombstone/meta (AssetDb.h) on the next scan.

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {
namespace importsort {

struct Settings {
  bool sort_imports = true;      // "Sort imports into type folders"
  bool keep_model_packs = true;  // "Keep source folder structure for model packs"
};

inline constexpr const char* kSettingsFile = ".tombstone/import.json";

Settings load_settings(const std::string& claim_dir);
bool save_settings(const std::string& claim_dir, const Settings& s, std::string* error = nullptr);

// Type folder (project-relative, no trailing slash) for a file name:
// "assets/Models", "assets/Textures", ..., "prefabs", "scripts".
std::string type_folder(const std::string& file_name);
// Every type folder, in menu order.
const std::vector<std::string>& type_folders();
bool is_texture_file(const std::string& path);

struct Result {
  std::vector<std::string> files;  // every project-relative file written
  std::vector<std::string> mains;  // the imported assets (models, loose files)
  std::vector<std::string> notes;
  std::vector<std::string> errors;
};

// Copies outside files (or whole folders) into the claim. dest_folder ""
// = sorted (or assets/ with sorting off); else exactly that folder
// (project-relative), keeping a dropped folder's structure. Existing
// files are never overwritten (name_2.png, Models/crate_2/). False when
// nothing could be imported.
bool import_files(const std::string& claim_dir, const std::vector<std::string>& sources,
                  const std::string& dest_folder, const Settings& settings, Result* out);

// Texture files a model names (absolute paths): glTF images, OBJ .mtl
// maps, COLLADA <init_from>; for other formats the images that sit in a
// "model pack" with it (its folder and textures/ / materials/ below it
// when it is the only model there, else images named after it).
std::vector<std::string> model_textures(const std::string& model_abs);

// OS file drops (glfwSetDropCallback in ts_admin): queued here, taken by
// the Asset Browser that is under the mouse (or the first one drawn).
void push_os_drop(const std::vector<std::string>& paths);
bool os_drop_pending();
std::vector<std::string> take_os_drop();

}  // namespace importsort
}  // namespace editor
}  // namespace tombstone
}  // namespace ts
