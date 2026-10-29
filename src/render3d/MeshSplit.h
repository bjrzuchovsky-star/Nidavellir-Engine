#pragma once

// Split a loaded model into one asset per mesh node (docs/model-import.md).
// Node transforms are already baked into the vertices; a skinned part is
// written in its rest pose, rigid, so each file stands on its own.
//
//   recentre  the pivot moves to the part's centre and `origin` is the
//             model-space centre to put back when the part is placed
//   keep      vertices stay where they were; the pivot is the model's origin

#include "render3d/Gltf.h"

#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

struct SplitPart {
  std::string name;      // file-safe, unique ("post", "arm_2")
  std::string node;      // node path it came from
  Vec3 origin;           // model-space centre before any recentre
  std::string rel_gltf;  // project-relative, set by the caller's folder
};

struct SplitWrite {
  bool recentre = false;
};

// How many separate parts a model would split into (collision / extra LOD
// meshes are already gone by the time load_model returns).
int model_part_count(const Model& model);

// Write one glTF (and any texture PNGs) per part into `dest_dir`.
// `written_abs` receives every file created. False when there is nothing
// to separate, or a file could not be written (`error` says which).
bool write_model_parts(const Model& model, const std::string& dest_dir, const SplitWrite& opt,
                       std::vector<SplitPart>* parts, std::vector<std::string>* written_abs,
                       std::string* error);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
