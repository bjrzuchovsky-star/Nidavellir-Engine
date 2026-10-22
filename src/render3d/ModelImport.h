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
// Import settings come from the Asset Browser's .meta (settings
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
// Reads the model's .meta when it exists (the claim's .tombstone/meta
// folder, else a "<path>.meta" sidecar; false = defaults used).
bool read_model_options(const std::string& path, ModelImportOptions* out);

// Load any supported model; settings from its .meta when present.
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
// Blender executable, first hit of: `override_path` (the asset's Blender
// path), TS_BLENDER, the editor's "Blender executable" setting
// (set_blender_setting), PATH, the registry (.blend open command,
// BlenderFoundation, Uninstall entries), Program Files\Blender Foundation\*,
// Microsoft Store / winget, Steam (every library in libraryfolders.vdf),
// the usual Linux / macOS places. "" when none; `looked` (optional) gets
// every place tried, for the error message.
std::string find_blender(const std::string& override_path,
                         std::vector<std::string>* looked = nullptr);
// The editor setting (Settings > Tools > Blender executable).
void set_blender_setting(const std::string& path);
std::string blender_setting();
// Library folders from a Steam libraryfolders.vdf (old and new layouts:
// "path" keys, or "1" "D:\\SteamLibrary" entries), unescaped.
std::vector<std::string> steam_library_folders_from_vdf(const std::string& vdf_text);

// Bake uniform scale and Z-up -> Y-up into a model (vertices, normals,
// tangents, skeleton roots / inverse binds, hulls, bounds).
void bake_import_transform(Model* m, float scale, bool z_up);
// Recompute Model::bounds from what is drawn: the vertices the triangles
// reference, skinned primitives posed by the rest pose (the palette the
// renderer uses), after the import transform.
void update_bounds(Model* m);
// The pre-0.1.1 bounds (every vertex, skinned ones unposed). Only for
// spotting colliders that were sized from them (scene migration).
Aabb legacy_vertex_bounds(const Model& m);
// Palette for one primitive from the skeleton palette (gathers through
// ModelPrimitive::joint_map; the full palette when the map is empty).
void primitive_palette(const ModelPrimitive& p, const std::vector<Mat4>& full,
                       std::vector<Mat4>* out);
// Every visual primitive merged into one rigid mesh in the rest pose
// (positions + indices; physics hulls / triangle colliders).
void rest_pose_mesh(const Model& m, MeshData* out);
// Moves collision meshes (UCX_ / UBX_ / UCP_ / USP_ prefixes, -colonly /
// _collision suffixes, a "collision" node) into collision_hulls (when
// `keep_hulls`) and keeps one level of detail of meshes tagged LOD<n>
// (_LOD1, LOD2 nodes...), so neither is drawn nor counted in the bounds.
// Returns how many primitives it took out. Sets Model::lods.
int split_special_meshes(Model* m, int lod, bool keep_hulls);
// Skeletons with more than kMaxBones joints: give each skinned primitive
// a joint_map (splitting primitives that use more than kMaxBones joints)
// so every draw's palette fits the renderer.
void limit_skin_joints(Model* m);
// Bone -> model matrices for Model::hitboxes: the rest pose (`palette`
// null / empty) or the pose a skeleton palette (skin_palette) carries.
void hitbox_matrices(const Model& m, const std::vector<Mat4>* palette, std::vector<Mat4>* out);
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
