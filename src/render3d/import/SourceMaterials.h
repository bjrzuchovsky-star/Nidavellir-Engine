#pragma once

// Material lookup for Source engine models (.mdl): the model names its
// materials ("crate") and $cdmaterials folders ("models/props/"); each is
// a materials/<cdmaterials><name>.vmt under some content root, whose
// $basetexture / $bumpmap name .vtf files under the same kind of roots.
//
// Content roots, searched in this order (first hit wins, $cdmaterials
// entries in the model's order, each tried against every root):
//   1. the asset's own "material_path" import setting (';' separated)
//   2. folders above the model: any with a materials/ folder, and the
//      parent of a "models" folder (an extracted addon / game folder)
//   3. the claim: its root, assets/, and every pack under assets/Models/
//      and assets/ (extracted .gma addons land in assets/Models/<Addon>/)
//   4. the editor's "Source content search folders" (Settings > Tools):
//      each folder, its garrysmod/ child, and their addons/* folders
// Missing materials keep the magenta checker; MaterialSource::searched
// says which files were looked for where, and the model's
// "material_overrides" import setting relinks them by hand.
//
// Provenance: written from the public VMT / VTF / $cdmaterials
// descriptions; nothing from the Source SDK. Read-only: never writes into
// the search folders (a game install stays untouched); no VPK reading.

#include "render3d/Gltf.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {
namespace source {

// Editor setting: "Source content search folders".
void set_content_search_folders(const std::vector<std::string>& folders);
std::vector<std::string> content_search_folders();
// ';' / newline separated list <-> folders (trimmed, empties dropped).
std::vector<std::string> split_folder_list(const std::string& text);
std::string join_folder_list(const std::vector<std::string>& folders);
// Read-only suggestions: Source game content folders found in Steam
// libraries (GarrysMod/garrysmod, Half-Life 2/hl2 ...). Never copied.
std::vector<std::string> detect_content_folders();

struct ContentRoot {
  std::string dir;
  std::string why;  // "model folder", "claim", "search folder"...
};
// Search order for a model (see above). Existing folders only, no repeats.
std::vector<ContentRoot> content_roots(const std::string& model_path,
                                       const std::vector<std::string>& material_paths);
// The claim (folder holding project.json) above a path, "" when none.
std::string claim_root_of(const std::string& path);

class MaterialResolver {
 public:
  MaterialResolver(std::vector<ContentRoot> roots, std::string model_dir);
  // One material of a model; `override_file` (may be "") is a relinked
  // .vmt / .vtf / image. Fills `src` (status, files, where searched).
  // Missing base textures get missing_texture_image().
  MaterialDesc resolve(const std::string& name, const std::vector<std::string>& cd_paths,
                       const std::string& override_file, MaterialSource* src);
  const std::vector<ContentRoot>& roots() const { return roots_; }

  // Lower-level, exposed for tests: finds materials/<rel><ext> (or <rel>
  // with ext under a root that is itself a materials folder).
  bool find(const std::string& rel, const char* ext, std::string* out) const;

 private:
  std::shared_ptr<const Image> image(const std::string& path, std::string* note);
  std::shared_ptr<const Image> texture(const std::string& name, const std::string& near_dir,
                                       std::string* found, std::string* note);
  void apply_vmt(const std::string& vmt_path, MaterialDesc* m, MaterialSource* src, std::string* note);

  std::vector<ContentRoot> roots_;
  std::string model_dir_;
  std::map<std::string, std::shared_ptr<const Image>> cache_;
};

// Normalised material / texture name: '\' -> '/', no leading '/', no
// "materials/" prefix, no .vmt / .vtf extension.
std::string clean_material_name(const std::string& name);

}  // namespace source
}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
