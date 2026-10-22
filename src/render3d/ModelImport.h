#pragma once

// Universal model import: every 3D model format the engine reads ends up
// as the same render3d::Model (primitives + materials + images, skeleton
// with skin weights, clips, collision hulls).
//
//   .gltf .glb          cgltf (render3d/Gltf.h)
//   .mdl (+ .vvd, .dx90.vtx / .dx80.vtx / .sw.vtx, optional .phy)
//                       Source engine models (GMod, HL2, TF2...), our own
//                       clean-room reader (import/SourceMdl.cpp), VMT / VTF
//                       materials (import/SourceVtf.cpp)
//   .obj .fbx .dae .3ds .ply .stl .x .lwo .ms3d .off .ase ...
//                       Assimp (BSD-3, fetched by hash; TS_WITH_ASSIMP)
//   .blend              Blender, run headless to export glTF (when found)
//
// Import settings come from the Asset Browser's "<file>.meta" (settings
// object) and are shared by every format: unit_scale and up_axis are baked
// into the vertices / skeleton / hulls; material_path adds texture search
// roots; lod picks a level of detail (Source); skin / body_groups pick
// Source variants; import_animations / generate_collider turn those off.
// Missing textures show a magenta checker.

#include "render3d/Gltf.h"
#include "scene/JsonTree.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ts {
namespace tombstone {
namespace render3d {

enum class UpAxis { Auto, Y, Z };

struct ModelImportOptions {
  float unit_scale = 1.0f;  // file units -> metres (Source: 0.0254)
  UpAxis up = UpAxis::Auto;  // Auto = the format's own (Source: Z up)
  std::vector<std::string> material_paths;  // extra roots ("a;b")
  int lod = 0;
  bool animations = true;
  bool collision = true;
  int skin = 0;                              // Source skin family
  std::map<std::string, int> body_groups;    // Source: group -> option
  std::string blender_path;                  // override for .blend
};

// Options from a .meta "settings" object (missing keys: format defaults).
ModelImportOptions model_options_from_settings(const json::Value& settings,
                                               const std::string& path);
// Settings a fresh .meta gets for this model file.
json::Value default_model_settings(const std::string& path);
// Reads "<path>.meta" when it exists (false = defaults used).
bool read_model_options(const std::string& path, ModelImportOptions* out);

// Load any supported model; settings from "<path>.meta" when present.
// Success with a non-empty *error = warnings (missing textures...).
bool load_model(const std::string& path, Model* out, std::string* error);
bool load_model(const std::string& path, const ModelImportOptions& opt, Model* out,
                std::string* error);

bool is_model_file(const std::string& path);   // by extension
const std::vector<std::string>& model_extensions();  // ".gltf", ".mdl"...
// Short format name for the extension ("glTF", "Source MDL", "FBX"...).
std::string model_format_name(const std::string& path);
// Other files the import reads (Source .vvd / .vtx / .phy, OBJ .mtl,
// glTF .bin) - the asset database hashes them too, for hot reload.
std::vector<std::string> model_dependencies(const std::string& path);

bool assimp_available();
// Blender executable: override, TS_BLENDER, PATH, the registry (Windows)
// and the usual install folders. "" when none.
std::string find_blender(const std::string& override_path);

// Bake uniform scale and Z-up -> Y-up into a model (vertices, normals,
// tangents, skeleton roots / inverse binds, hulls, bounds).
void bake_import_transform(Model* m, float scale, bool z_up);
// Recompute Model::bounds from the primitives.
void update_bounds(Model* m);
// 8x8 magenta / black checker shown where a texture is missing.
std::shared_ptr<const Image> missing_texture_image();

// One-line rights note shown by the import UI.
inline constexpr const char* kModelRightsNote =
    "You are responsible for having the rights to any model you import.";

// Loaders (import/*.cpp), called by load_model.
bool load_source_mdl(const std::string& path, const ModelImportOptions& opt, Model* out,
                     std::string* error);
bool load_assimp_model(const std::string& path, const ModelImportOptions& opt, Model* out,
                       std::string* error);
bool load_blend_model(const std::string& path, const ModelImportOptions& opt, Model* out,
                      std::string* error);

}  // namespace render3d
}  // namespace tombstone
}  // namespace ts
