#pragma once

// Split a model asset into one glTF per mesh node, each with its own .meta
// in the claim's metadata folder (editor/assets/AssetDb.h). The new files
// sit under assets/ like any other asset, so the Vault and a posse pick
// them up on the next pass.

#include "editor/assets/AssetDb.h"
#include "render3d/Math3D.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace editor {

struct ModelSplitOptions {
  bool recentre = false;      // pivot at the part's centre
  bool fit_collider = true;   // each part keeps a fitted collider
};

struct ModelSplitFile {
  std::string rel;  // project-relative (the glTF, its textures, its .meta)
  std::string bytes;
};

struct ModelSplitResult {
  std::vector<std::string> rels;  // the part glTFs
  std::vector<std::string> names;
  std::vector<render3d::Vec3> origins;  // model-space centre, before recentre
  std::vector<ModelSplitFile> files;
  std::string error;
};

// False when the model is one piece, or a file could not be written.
bool split_model_asset(assetdb::AssetDb& db, const std::string& rel, const ModelSplitOptions& opt,
                       ModelSplitResult* out);

}  // namespace editor
}  // namespace tombstone
}  // namespace ts
